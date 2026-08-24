// ── D-TYPEKIND-PASCALCASE-SPELLINGS-HAVE-TWO-OWNERS ──────────────────────
//    `TypeKind`'s forty PascalCase spellings, and the twenty-kind PRIMITIVE
//    subset the config surface accepts, each reduced to ONE owner.
// ─────────────────────────────────────────────────────────────────────────
//
// THE CLASS. One enum, three independent hand-lists of its spellings and two
// independent hand-lists of one subset of it:
//
//   * `lir/lir_text.cpp`'s `typeKindName` — a forty-arm switch producing the
//     `.dsslir` literal-pool `core <Kind>` round-trip TAG, with a walk over it
//     as the inverse. A ROUTER in both directions, not a reporter.
//   * `core/types/type_lattice/type_reintern.cpp`'s `typeKindName` — a
//     forty-ONE-arm switch producing an abort-message name.
//   * `core/types/grammar_schema_json.cpp`'s `kGrammarCoreTypeTable` — twenty
//     of the same spellings, deciding what a `.lang.json` `"core"` field may
//     name. Also a router: its result is handed to `TypeInterner::primitive`.
//   * `type_reintern.cpp`'s `isPrimitiveKind` — the same twenty KINDS again,
//     as a predicate, gating the leaf-rebuild arm.
//
// ✔MEASURED at the row: the first two agreed on all forty real kinds and
// DISAGREED on `Count_` (`"?"` versus `"Count_"`); the twenty config spellings
// were byte-identical to the LIR ones; and the twenty config KINDS were the
// same twenty, in the same order, as `isPrimitiveKind`'s arms.
//
// ★★ THE ARTEFACT THAT KEPT IT INVISIBLE WAS A COMMENT, not the code.
// `lir_text.cpp` carried *"Names match the sibling table in
// `type_lattice/type_reintern.cpp`, which never drifted"* — a certificate about
// another file, placed where the next reader would take it instead of checking,
// and false on the day it was measured.
//
// ★★★ WHY THE EXPECTATIONS BELOW ARE LITERALS, WITH LITERAL COUNTS. A pin whose
// expectation is projected off the SAME table the code renders moves BOTH HALVES
// TOGETHER: rename a row and the message follows it, and "the message names
// every row" stays green while the vocabulary silently changed under every
// document in the tree. ✔MEASURED LIVE in cycle P28 by the sibling lane's M4b
// mutant, which went green for exactly that reason. So every set below is
// written out here, and no arm compares against `allNames(...)`.
// ⛔ In particular a count is NEVER spelled `kTable.rows.size()` or
// `size() - 1`: that is `x == x` and cannot fail
// (D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY).
//
// WHAT THE ARMS ASSERT, and it takes five because "one owner" is not one claim:
//   (A) THE TABLE IS WHAT THIS FILE SAYS IT IS — forty rows, these spellings,
//       in this order, at a literal count. The arm a row-rename mutant reds,
//       and the only one that cannot be satisfied by moving code around.
//   (B) `Count_` HAS NO SPELLING AND RESOLVES FROM NOTHING — the row's own
//       question, answered rather than deferred. It is the enum-cardinality
//       sentinel; a row for it would give BOTH routers a resolution for a value
//       no `TypeRecord` can carry.
//   (C) THE `.dsslir` TAG IS TOTAL AND INJECTIVE OVER ALL FORTY — emitted text
//       carries each literal spelling and the reparse reconstructs each kind
//       distinctly. This is the arm that proves `lir_text.cpp` PROJECTS through
//       the table rather than merely coexisting with it, and it covers the
//       twenty kinds the config surface never sees.
//   (D) THE CONFIG SURFACE IS A FILTERED PROJECTION, NOT A COPY AND NOT THE
//       IDENTITY — the loader accepts each of the twenty primitive spellings and
//       REFUSES each of the twenty non-primitive ones BY NAME, at that key's own
//       JSON pointer. Both lists literal, both counts literal.
//   (E) BOTH TEXT-TIER REFUSALS STATE THEIR ACCEPTED SET — the `.lang.json`
//       one names twenty, the `.dsslir` one names forty, and neither may quote a
//       spelling outside its own set.
//
// ⚠ MUST run through ctest, never a bare `.exe`: `findShippedConfig` and
// `TargetSchema::loadShipped` walk the cwd unless `DSS_CONFIG_ROOT` is set, and
// only `dss_add_test` sets it.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/enum_name_table.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "lir/lir.hpp"
#include "lir/lir_literal_pool.hpp"
#include "lir/lir_text.hpp"
#include "vocabulary_projection_probe.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

using ::dss::test_support::at;
using ::dss::test_support::quotedTokens;
using ::dss::test_support::shippedLanguageDoc;
using ::dss::test_support::summarize;

// ── THE SETS, STATED HERE ────────────────────────────────────────────────
//
// Written out rather than projected, for the reason the file header gives. The
// ORDER is the enum's declaration order, which is also the table's, so arm (A)
// catches a REORDER as well as a rename — a reorder is not cosmetic for an
// `EnumNameTable`, whose `name()` fall-back is row 0.
constexpr std::string_view kAllTypeKindNames[] = {
    "Bool",
    "I8", "I16", "I32", "I64", "I128",
    "U8", "U16", "U32", "U64", "U128",
    "F16", "F32", "F64", "F80", "F128",
    "Char", "Byte", "Void",
    "Struct", "Union", "Tuple", "Array", "Slice",
    "Enum",
    "Vector", "Matrix",
    "Ptr", "Ref", "FnPtr", "Nullable", "Optional",
    "FnSig",
    "Param", "Bind",
    "Extension",
    "VolatileQual",
    "NullptrT",
    "BitInt",
    "Complex",
};
constexpr std::size_t kTypeKindNameCount = 40;

// The twenty a `.lang.json` `"core"` field may name: the LEAF kinds
// `TypeInterner::primitive(k)` can realize from the kind alone.
constexpr std::string_view kPrimitiveTypeKindNames[] = {
    "Bool",
    "I8", "I16", "I32", "I64", "I128",
    "U8", "U16", "U32", "U64", "U128",
    "F16", "F32", "F64", "F80", "F128",
    "Char", "Byte", "Void",
    "NullptrT",
};
constexpr std::size_t kPrimitiveTypeKindCount = 20;

// The other twenty: real `TypeKind` spellings the LATTICE owns and the config
// surface must REFUSE. Stated as its own list rather than as a set difference —
// a difference computed from the two lists above would go green if a name were
// dropped from both.
constexpr std::string_view kNonPrimitiveTypeKindNames[] = {
    "Struct", "Union", "Tuple", "Array", "Slice",
    "Enum",
    "Vector", "Matrix",
    "Ptr", "Ref", "FnPtr", "Nullable", "Optional",
    "FnSig",
    "Param", "Bind",
    "Extension",
    "VolatileQual",
    "BitInt",
    "Complex",
};
constexpr std::size_t kNonPrimitiveTypeKindCount = 20;

// A spelling no vocabulary in this tree claims. Deliberately ugly: a probe that
// collided with a real name would make the negative arms pass for the wrong
// reason.
constexpr char const* kBadSpelling = "zzNotAnySpellingAtAll";

// The `.lang.json` pointer whose value is a `core` name. Named as a constant so
// the two arms that use it cannot drift onto different keys, and guarded by
// `at()` so a pointer that stops resolving FAILS rather than probing nothing.
constexpr std::string_view kCorePointer = "/semantics/builtinTypes/0/core";

[[nodiscard]] std::optional<std::string>
messageAt(auto const& diags, std::string_view pointer) {
    for (auto const& d : diags) {
        if (d.path == pointer) return d.message;
    }
    return std::nullopt;
}

[[nodiscard]] std::shared_ptr<TargetSchema> shippedX86() {
    auto target = TargetSchema::loadShipped("x86_64");
    if (!target) {
        // THROW, never `std::abort()`: abort kills the whole test BINARY,
        // so every sibling test in this executable loses its verdict and
        // the harness cannot say which unit failed. GoogleTest reports a
        // throw as a failure of this ONE test. Rule and measurement in
        // tests/test_support/repo_root.hpp; machine-checked by
        // check-no-abort-in-tests, whose ratchet this had breached.
        throw std::runtime_error{"loadShipped(x86_64) failed"};
    }
    return *target;
}

[[nodiscard]] std::string diagText(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) {
        s += std::format("  [{}] {}\n", static_cast<int>(d.code), d.actual);
    }
    return s.empty() ? std::string{"<no diagnostics>"} : s;
}

}  // namespace

// ── (A) THE TABLE IS WHAT THIS FILE SAYS IT IS ───────────────────────────
TEST(TypeKindVocabulary, TheTableHoldsExactlyTheseFortySpellingsInThisOrder) {
    ASSERT_EQ(std::size(kAllTypeKindNames), kTypeKindNameCount)
        << "the literal list and the literal count disagree — fix the list, "
           "never the count";
    // The enum's own cardinality is an INDEPENDENT fact from the list above, so
    // this is a real comparison rather than a restatement.
    ASSERT_EQ(static_cast<std::size_t>(TypeKind::Count_), kTypeKindNameCount)
        << "a TypeKind enumerator was added or removed. Every set in this file "
           "has to be re-decided, starting with whether the new kind is "
           "primitive.";

    auto const names = allNames(kTypeKindNameTable);
    ASSERT_EQ(names.size(), kTypeKindNameCount);
    for (std::size_t i = 0; i < kTypeKindNameCount; ++i) {
        EXPECT_EQ(names[i], kAllTypeKindNames[i])
            << "row " << i << " of kTypeKindNameTable is '" << names[i]
            << "', this file says '" << kAllTypeKindNames[i]
            << "'. A renamed row re-spells the `.dsslir` `core` tag and the "
               "`.lang.json` `core` vocabulary at once.";
    }

    // Both directions, per name — the table is a ROUTER on both sides.
    for (std::size_t i = 0; i < kTypeKindNameCount; ++i) {
        auto const k = typeKindFromName(kAllTypeKindNames[i]);
        ASSERT_TRUE(k.has_value())
            << "'" << kAllTypeKindNames[i] << "' resolves to no TypeKind";
        EXPECT_EQ(typeKindNameOrEmpty(*k), kAllTypeKindNames[i]);
        EXPECT_EQ(static_cast<std::size_t>(*k), i)
            << "'" << kAllTypeKindNames[i]
            << "' resolved to a kind at a different ordinal than its position "
               "in the declaration order this file states";
    }
}

// ── (B) `Count_` HAS NO SPELLING, AND THAT IS THE POINT ──────────────────
TEST(TypeKindVocabulary, TheCardinalitySentinelIsNeitherNamedNorResolvable) {
    EXPECT_TRUE(typeKindNameOrEmpty(TypeKind::Count_).empty())
        << "`Count_` is the enum-cardinality sentinel, not a type. It renders "
           "EMPTY so each consumer states its own 'no spelling' sentence; a "
           "name here would be a name for a value no TypeRecord can carry.";
    EXPECT_FALSE(typeKindFromName("Count_").has_value())
        << "`fromName(\"Count_\")` must not resolve: BOTH consumers of this "
           "table are routers, so a resolution would let `.dsslir` text mint a "
           "`core Count_` literal-pool tag and a `.lang.json` declare "
           "\"core\": \"Count_\" — accepted at load, fatal downstream.";
    EXPECT_FALSE(typeKindFromName("?").has_value())
        << "`?` is `lir_text.cpp`'s 'no spelling' rendering, not a name. If it "
           "ever resolved, the tag would stop being injective — which is the "
           "state VolatileQual and NullptrT were in for their whole lifetime.";
    EXPECT_FALSE(typeKindFromName("").has_value())
        << "an empty spelling must resolve to nothing — an under-filled "
           "EnumNameTable is legal C++ and would make \"\" select row 0's kind";

    // TOTALITY, at run time as well as at the header's static_assert: every
    // ordinal BELOW the sentinel is named. `Count_` is the one hole, and it is
    // deliberate.
    std::size_t named = 0;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(TypeKind::Count_); ++i) {
        auto const k = static_cast<TypeKind>(i);
        EXPECT_FALSE(typeKindNameOrEmpty(k).empty())
            << "TypeKind ordinal " << i
            << " has no row in kTypeKindNameTable, so it renders as the "
               "caller's 'no spelling' sentinel while being a REAL kind";
        if (!typeKindNameOrEmpty(k).empty()) ++named;
    }
    EXPECT_EQ(named, kTypeKindNameCount);
}

// ── (C) THE `.dsslir` TAG PROJECTS THROUGH THE TABLE, TOTALLY ────────────
//
// One literal-pool entry per kind, emitted and reparsed. This is the arm that
// says `lir_text.cpp` READS the table: arm (A) would stay green if that file
// kept its own switch beside it, which is precisely the state the row names.
TEST(TypeKindVocabulary, EveryTypeKindRoundTripsThroughTheDsslirCoreTag) {
    auto const sch = shippedX86();
    LirBuilder b{*sch};
    std::vector<std::uint32_t> idx;
    idx.reserve(kTypeKindNameCount);
    for (std::size_t i = 0; i < kTypeKindNameCount; ++i) {
        LirLiteralValue lit;
        lit.value = static_cast<std::uint64_t>(i);
        lit.core  = static_cast<TypeKind>(i);
        idx.push_back(b.literalPoolAdd(std::move(lit)));
    }
    Lir lir = std::move(b).finish();

    DiagnosticReporter repEmit, repParse;
    LirTextContext     ctx;
    std::string const  text = emitLir(lir, *sch, ctx, repEmit);

    EXPECT_EQ(text.find("core ?"), std::string::npos)
        << "no VALID kind may render the 'no spelling' sentinel — `?` resolves "
           "through no row, so a kind that emits it is a lossy tag by "
           "construction. Emitted:\n"
        << text;

    // ⚠ THE WHOLE POOL LINE, NOT A SUBSTRING. ✔MEASURED on mutant M1, which
    // renamed the `NullptrT` row to `NullptrTz`: this arm stayed GREEN because
    // `find("core NullptrT")` matches inside `core NullptrTz`. A prefix search
    // cannot see a row that grew a suffix — which is the commonest shape of a
    // spelling edit. Matching `  lit#<i> = u64 <i> core <Name>\n` pins the
    // index, the value and the tag together, and the trailing newline makes the
    // spelling a whole token.
    for (std::size_t i = 0; i < kTypeKindNameCount; ++i) {
        EXPECT_NE(text.find(std::format("  lit#{} = u64 {} core {}\n", i, i,
                                        kAllTypeKindNames[i])),
                  std::string::npos)
            << "the emitted `.dsslir` never carries the pool line `  lit#" << i
            << " = u64 " << i << " core " << kAllTypeKindNames[i]
            << "`, the spelling this file states for ordinal " << i
            << ".\nemitted:\n"
            << text;
    }

    auto result = parseLir(text, *sch, repParse);
    ASSERT_TRUE(result != nullptr && result->ok) << diagText(repParse);
    ASSERT_EQ(result->lir.literalPool().size(), kTypeKindNameCount);
    for (std::size_t i = 0; i < kTypeKindNameCount; ++i) {
        EXPECT_EQ(result->lir.literalValue(idx[i]).core,
                  static_cast<TypeKind>(i))
            << "pool entry " << i << " ('" << kAllTypeKindNames[i]
            << "') did not reconstruct as its own kind — two kinds sharing one "
               "tag is exactly the un-injective state this table removes";
    }
}

// ── (D) THE CONFIG SURFACE IS A FILTERED PROJECTION ──────────────────────
//
// Not a copy of the lattice table (it must refuse `Struct`) and not a hand-list
// of its own (it must follow a renamed row). Proved from the OUTSIDE, through
// the shipped document and the real loader, so nothing here can be satisfied by
// reading the loader's own array back.
TEST(TypeKindVocabulary, TheCoreFieldAcceptsExactlyTheTwentyPrimitiveSpellings) {
    ASSERT_EQ(std::size(kPrimitiveTypeKindNames), kPrimitiveTypeKindCount);
    ASSERT_EQ(std::size(kNonPrimitiveTypeKindNames), kNonPrimitiveTypeKindCount);
    ASSERT_EQ(kPrimitiveTypeKindCount + kNonPrimitiveTypeKindCount,
              kTypeKindNameCount)
        << "the two halves must partition the lattice's spellings, or one of "
           "them is silently not being tested";

    auto doc = shippedLanguageDoc("c");
    // The pointer must RESOLVE, or both halves below probe nothing.
    auto& core = at(doc, kCorePointer, "semantics.builtinTypes[0].core");
    ASSERT_TRUE(core.is_string())
        << "the shipped document must write a string `core` at " << kCorePointer
        << ", or this pin asserts nothing";

    // The document must load CLEAN before any mutation, otherwise a refusal
    // below could be about something else entirely.
    {
        auto const clean = GrammarSchema::loadFromText(doc.dump(), "c");
        ASSERT_TRUE(clean.has_value()) << summarize(clean.error());
    }

    // ACCEPTED: every one of the twenty.
    for (auto const& name : kPrimitiveTypeKindNames) {
        SCOPED_TRACE(std::string{"accepted: "} + std::string{name});
        at(doc, kCorePointer, "core") = std::string{name};
        auto const r = GrammarSchema::loadFromText(doc.dump(), "c");
        ASSERT_TRUE(r.has_value())
            << "the loader refused '" << name
            << "', a spelling its own refusal advertises:\n"
            << summarize(r.error());
    }

    // REFUSED: every one of the other twenty — real lattice spellings that
    // `TypeInterner::primitive` cannot realize from the kind alone.
    for (auto const& name : kNonPrimitiveTypeKindNames) {
        SCOPED_TRACE(std::string{"refused: "} + std::string{name});
        at(doc, kCorePointer, "core") = std::string{name};
        auto const r = GrammarSchema::loadFromText(doc.dump(), "c");
        ASSERT_FALSE(r.has_value())
            << "the loader ACCEPTED '" << name
            << "'. It is a `TypeKind` the lattice names but not one "
               "`primitive(k)` can build, so it would be accepted at load and "
               "fatal downstream — the filter has become the identity.";
        auto const msg = messageAt(r.error(), kCorePointer);
        ASSERT_TRUE(msg.has_value())
            << "the refusal is not at the key's own JSON pointer, so a sibling "
               "diagnostic could be covering for it:\n"
            << summarize(r.error());
    }
}

// ── (E) BOTH REFUSALS STATE THEIR OWN ACCEPTED SET ───────────────────────
TEST(TypeKindVocabulary, TheLangJsonCoreRefusalNamesItsTwentyAndNoOthers) {
    auto doc = shippedLanguageDoc("c");
    at(doc, kCorePointer, "semantics.builtinTypes[0].core") = kBadSpelling;
    auto const r = GrammarSchema::loadFromText(doc.dump(), "c");
    ASSERT_FALSE(r.has_value()) << "an unknown `core` spelling must FAIL";
    auto const msg = messageAt(r.error(), kCorePointer);
    ASSERT_TRUE(msg.has_value()) << summarize(r.error());

    auto const quoted = quotedTokens(*msg);
    // COMPLETENESS — every accepted spelling is advertised.
    for (auto const& name : kPrimitiveTypeKindNames) {
        bool named = false;
        for (auto const& q : quoted) {
            if (q == name) { named = true; break; }
        }
        EXPECT_TRUE(named) << "the refusal does not name '" << name
                           << "', which the loader accepts.\nmessage was:\n"
                           << *msg;
    }
    // HONESTY — and nothing it refuses. A message WIDER than its check sends an
    // author to write a value that is then rejected.
    for (auto const& name : kNonPrimitiveTypeKindNames) {
        for (auto const& q : quoted) {
            EXPECT_NE(q, name)
                << "the refusal advertises '" << name
                << "', which it does not accept.\nmessage was:\n"
                << *msg;
        }
    }
}

TEST(TypeKindVocabulary, TheDsslirCoreRefusalNamesAllFortyLatticeSpellings) {
    auto const sch = shippedX86();
    // The fixture is a REAL emitted module with its tag rewritten, not a
    // hand-typed `.dsslir` header: a hand-typed one that stopped parsing for an
    // unrelated reason would make this arm pass on the wrong diagnostic.
    LirBuilder b{*sch};
    LirLiteralValue lit;
    lit.value = std::uint64_t{0};
    lit.core  = TypeKind::I32;
    std::uint32_t const only = b.literalPoolAdd(std::move(lit));
    ASSERT_EQ(only, 0u);
    Lir lir = std::move(b).finish();

    DiagnosticReporter repEmit;
    LirTextContext     ctx;
    std::string        text = emitLir(lir, *sch, ctx, repEmit);
    auto const         tagAt = text.find("core I32");
    ASSERT_NE(tagAt, std::string::npos)
        << "the emitter did not write the tag this arm rewrites:\n" << text;
    text.replace(tagAt, std::string_view{"core I32"}.size(),
                 std::string{"core "} + kBadSpelling);

    DiagnosticReporter rep;
    auto const         result = parseLir(text, *sch, rep);
    (void)result;

    std::string const* found = nullptr;
    for (auto const& d : rep.all()) {
        if (d.actual.find("unknown TypeKind") != std::string::npos) {
            found = &d.actual;
            break;
        }
    }
    ASSERT_NE(found, nullptr)
        << "the `.dsslir` reader did not refuse an unknown `core` tag by "
           "name:\n"
        << diagText(rep);

    auto const quoted = quotedTokens(*found);
    for (auto const& name : kAllTypeKindNames) {
        bool named = false;
        for (auto const& q : quoted) {
            if (q == name) { named = true; break; }
        }
        EXPECT_TRUE(named)
            << "the `.dsslir` `core` refusal does not name '" << name
            << "', a tag it accepts. It named NOTHING before this row — an "
               "author who misspelled a kind learned only that it was "
               "wrong.\nmessage was:\n"
            << *found;
    }
}
