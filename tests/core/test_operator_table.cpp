#include "core/types/grammar_schema.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>
#include <type_traits>

using namespace dss;

namespace {

// Fixture exercises every arity slot, plus a lexeme (`-`) that occupies
// two slots simultaneously (Infix at 50, Prefix at 70).
constexpr std::string_view kOpsConfig = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "OpsTest", "version": "0.1.0" },

  "tokens": {
    "+":  [{ "kind": "PlusOp"     }],
    "-":  [{ "kind": "MinusOp"    }],
    "*":  [{ "kind": "StarOp"     }],
    "/":  [{ "kind": "SlashOp"    }],
    "=":  [{ "kind": "AssignOp"   }],
    "!":  [{ "kind": "BangOp"     }],
    "++": [{ "kind": "PlusPlusOp" }]
  },

  "operators": {
    "groups": [
      { "precedence": 10, "associativity": "right",                       "operators": ["="]       },
      { "precedence": 50, "associativity": "left",                        "operators": ["+", "-"]  },
      { "precedence": 60, "associativity": "left",                        "operators": ["*", "/"]  },
      { "precedence": 70, "associativity": "right", "arity": "prefix",    "operators": ["!", "-"]  },
      { "precedence": 80, "associativity": "left",  "arity": "postfix",   "operators": ["++"]      }
    ]
  },

  "shapes": { "root": { "sequence": [ "Identifier" ] } }
})JSON";

[[nodiscard]] std::shared_ptr<GrammarSchema const> loadOpsConfig() {
    auto loaded = GrammarSchema::loadFromText(kOpsConfig);
    EXPECT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    return loaded.has_value() ? *loaded : nullptr;
}

[[nodiscard]] SchemaTokenId kindId(GrammarSchema const& s, std::string_view name) {
    return s.schemaTokens().find(name);
}

} // namespace

// ── Compile-time invariants on the new types ──────────────────────────────
//
// Sizes are referenced by OperatorTable::key()'s packing budget; the
// header declares static_asserts for all of these, but pinning them
// from the test side too catches a header refactor that accidentally
// relaxes them.

static_assert(sizeof(OperatorAssoc) == 1);
static_assert(sizeof(OperatorArity) == 1);
static_assert(std::is_trivially_copyable_v<OperatorTable::Entry>);

// ── Schema-driven lookups ─────────────────────────────────────────────────

TEST(OperatorTable, InfixPlusReturnsPrecedence50) {
    auto schema = loadOpsConfig();
    ASSERT_NE(schema, nullptr);
    auto e = schema->operatorTable().lookup(kindId(*schema, "PlusOp"),
                                            OperatorArity::Infix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->precedence, 50);
    EXPECT_EQ(e->associativity, OperatorAssoc::Left);
}

TEST(OperatorTable, AssignIsRightAssoc) {
    auto schema = loadOpsConfig();
    ASSERT_NE(schema, nullptr);
    auto e = schema->operatorTable().lookup(kindId(*schema, "AssignOp"),
                                            OperatorArity::Infix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->precedence, 10);
    EXPECT_EQ(e->associativity, OperatorAssoc::Right);
}

// Same lexeme can be both prefix AND infix with distinct entries.
TEST(OperatorTable, MinusHasDistinctPrefixAndInfixEntries) {
    auto schema = loadOpsConfig();
    ASSERT_NE(schema, nullptr);
    const auto minus = kindId(*schema, "MinusOp");

    auto infix = schema->operatorTable().lookup(minus, OperatorArity::Infix);
    ASSERT_TRUE(infix.has_value());
    EXPECT_EQ(infix->precedence, 50);
    EXPECT_EQ(infix->associativity, OperatorAssoc::Left);

    auto prefix = schema->operatorTable().lookup(minus, OperatorArity::Prefix);
    ASSERT_TRUE(prefix.has_value());
    EXPECT_EQ(prefix->precedence, 70);
    EXPECT_EQ(prefix->associativity, OperatorAssoc::Right);

    // Postfix slot unoccupied — not just "missing", definitively no entry.
    EXPECT_FALSE(schema->operatorTable()
                     .lookup(minus, OperatorArity::Postfix).has_value());
}

TEST(OperatorTable, IdentifierIsNotAnOperator) {
    auto schema = loadOpsConfig();
    ASSERT_NE(schema, nullptr);
    EXPECT_FALSE(schema->operatorTable()
                     .lookup(kindId(*schema, "Identifier"), OperatorArity::Infix).has_value());
}

TEST(OperatorTable, PostfixIncrement) {
    auto schema = loadOpsConfig();
    ASSERT_NE(schema, nullptr);
    const auto inc = kindId(*schema, "PlusPlusOp");
    auto e = schema->operatorTable().lookup(inc, OperatorArity::Postfix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->precedence, 80);
    EXPECT_EQ(e->associativity, OperatorAssoc::Left);
    EXPECT_FALSE(schema->operatorTable().lookup(inc, OperatorArity::Infix).has_value());
    EXPECT_FALSE(schema->operatorTable().lookup(inc, OperatorArity::Prefix).has_value());
}

TEST(OperatorTable, DefaultArityIsInfix) {
    auto schema = loadOpsConfig();
    ASSERT_NE(schema, nullptr);
    // `*` group has no `arity` field — must land in the Infix slot.
    auto e = schema->operatorTable().lookup(kindId(*schema, "StarOp"),
                                            OperatorArity::Infix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->precedence, 60);
}

TEST(OperatorTable, DefaultAssociativityIsNone) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "$": [{ "kind": "DollarOp" }] },
      "operators": { "groups": [ { "precedence": 5, "operators": ["$"] } ] },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto e = (*loaded)->operatorTable().lookup(
        (*loaded)->schemaTokens().find("DollarOp"), OperatorArity::Infix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->associativity, OperatorAssoc::None);
}

// ── Multi-meaning disambiguation ──────────────────────────────────────────

TEST(OperatorTable, MultiMeaningLexemeWithoutKindIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "<": [{ "kind": "LtOp" }, { "kind": "GenericOpener", "opensScope": "Generic" }]
      },
      "operators": {
        "groups": [ { "precedence": 80, "associativity": "left", "operators": ["<"] } ]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    auto const& diags = loaded.error();
    auto it = std::ranges::find_if(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidPrecedenceTable;
    });
    ASSERT_NE(it, diags.end());
    EXPECT_NE(it->message.find("meanings"), std::string::npos)
        << "got: " << it->message;
    EXPECT_NE(it->message.find("kind"), std::string::npos)
        << "got: " << it->message;
    EXPECT_NE(it->message.find("'<'"), std::string::npos)
        << "message should name the offending lexeme; got: " << it->message;
}

TEST(OperatorTable, MultiMeaningLexemeWithKindResolves) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "<": [{ "kind": "LtOp" }, { "kind": "GenericOpener", "opensScope": "Generic" }]
      },
      "operators": {
        "groups": [
          { "precedence": 80, "associativity": "left", "kind": "LtOp", "operators": ["<"] }
        ]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& schema = **loaded;
    auto e = schema.operatorTable().lookup(schema.schemaTokens().find("LtOp"),
                                           OperatorArity::Infix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->precedence, 80);

    // The OTHER meaning (GenericOpener) does NOT get the operator entry —
    // explicit `kind` is exclusive, not additive.
    EXPECT_FALSE(schema.operatorTable()
                     .lookup(schema.schemaTokens().find("GenericOpener"),
                             OperatorArity::Infix).has_value());
}

TEST(OperatorTable, KindMustBeAMeaningOfTheLexeme) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        "-": [{ "kind": "MinusOp" }]
      },
      "operators": {
        "groups": [
          { "precedence": 50, "kind": "MinusOp", "operators": ["+"] }
        ]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidPrecedenceTable;
    }));
}

// Single-meaning lexeme with an explicit `kind` that DOES match the
// single meaning loads cleanly — the explicit-kind path is the same
// validation path the multi-meaning case takes.
TEST(OperatorTable, SingleMeaningLexemeWithMatchingKindResolves) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "operators": {
        "groups": [
          { "precedence": 50, "associativity": "left", "kind": "PlusOp", "operators": ["+"] }
        ]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto e = (*loaded)->operatorTable().lookup(
        (*loaded)->schemaTokens().find("PlusOp"), OperatorArity::Infix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->precedence, 50);
}

TEST(OperatorTable, SingleMeaningLexemeWithMismatchedKindIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        "-": [{ "kind": "MinusOp" }]
      },
      "operators": {
        "groups": [
          { "precedence": 50, "kind": "PlusOp", "operators": ["-"] }
        ]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidPrecedenceTable;
    }));
}

// ── Loader error paths ────────────────────────────────────────────────────

TEST(OperatorTable, UnknownOperatorLexemeIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "operators": {
        "groups": [ { "precedence": 50, "operators": ["**"] } ]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidPrecedenceTable;
    }));
}

TEST(OperatorTable, InvalidAssociativityIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "operators": {
        "groups": [ { "precedence": 50, "associativity": "sideways", "operators": ["+"] } ]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidPrecedenceTable;
    }));
}

TEST(OperatorTable, InvalidArityIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "operators": {
        "groups": [ { "precedence": 50, "arity": "circumfix", "operators": ["+"] } ]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidPrecedenceTable;
    }));
}

// Two groups stamping the same (id, arity) slot silently last-wins via
// insert_or_assign in the table. The loader rejects so the bug stays
// visible at config-load time rather than as a wrong precedence value
// at parse time.
TEST(OperatorTable, DuplicateOperatorEntryIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "operators": {
        "groups": [
          { "precedence": 50, "operators": ["+"] },
          { "precedence": 60, "operators": ["+"] }
        ]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    auto const& diags = loaded.error();
    auto it = std::ranges::find_if(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidPrecedenceTable;
    });
    ASSERT_NE(it, diags.end());
    EXPECT_NE(it->message.find("declared twice"), std::string::npos)
        << "got: " << it->message;
}

// Same lexeme in two groups with DIFFERENT arities is fine — the
// (id, arity) key is unique. This is the `-` case in c-subset.
TEST(OperatorTable, SameLexemeDifferentArityIsNotADuplicate) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "-": [{ "kind": "MinusOp" }] },
      "operators": {
        "groups": [
          { "precedence": 50,                  "operators": ["-"] },
          { "precedence": 70, "arity": "prefix", "operators": ["-"] }
        ]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const& t = (*loaded)->operatorTable();
    const auto m = (*loaded)->schemaTokens().find("MinusOp");
    EXPECT_TRUE(t.lookup(m, OperatorArity::Infix).has_value());
    EXPECT_TRUE(t.lookup(m, OperatorArity::Prefix).has_value());
}

// ── Backward compat ───────────────────────────────────────────────────────

TEST(OperatorTable, AbsentSectionYieldsEmptyTable) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE((*loaded)->operatorTable().empty());
    EXPECT_EQ((*loaded)->operatorTable().size(), 0u);
}

// ── `expr` shape kind ─────────────────────────────────────────────────────

TEST(OperatorTable, ExprShapeAtomResolvesToKnownRule) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": {
        "root":       { "sequence": [ "expression" ] },
        "expression": { "expr":     { "atom": "primary" } },
        "primary":    { "sequence": [ "Identifier" ] }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
}

TEST(OperatorTable, ExprShapeWithUnknownAtomIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "shapes": {
        "root":       { "sequence": [ "expression" ] },
        "expression": { "expr":     { "atom": "doesNotExist" } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_UnknownShape;
    }));
}

TEST(OperatorTable, ExprShapeWithoutAtomIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "shapes": {
        "root":       { "sequence": [ "expression" ] },
        "expression": { "expr":     { "minPrecedence": 0 } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_MissingField;
    }));
}

// A typo in `minPrecedence` should NOT silently load with the default 0 —
// the loader rejects unknown keys so the typo is visible.
TEST(OperatorTable, ExprShapeWithUnknownKeyIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "shapes": {
        "root":       { "sequence": [ "expression" ] },
        "expression": { "expr":     { "atom": "primary", "minPrecdence": 0 } },
        "primary":    { "sequence": [ "Identifier" ] }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    auto const& diags = loaded.error();
    auto it = std::ranges::find_if(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_UnknownShape;
    });
    ASSERT_NE(it, diags.end());
    EXPECT_NE(it->message.find("minPrecdence"), std::string::npos)
        << "got: " << it->message;
}

// `sequence` and `expr` in the same shape body would silently run both
// branches. Reject so the config bug is visible.
TEST(OperatorTable, ShapeBodyWithMultipleKindsIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "shapes": {
        "root":       { "sequence": [ "expression" ] },
        "expression": { "sequence": [ "Identifier" ], "expr": { "atom": "primary" } },
        "primary":    { "sequence": [ "Identifier" ] }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    auto const& diags = loaded.error();
    auto it = std::ranges::find_if(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_UnknownShape;
    });
    ASSERT_NE(it, diags.end());
    EXPECT_NE(it->message.find("multiple kinds"), std::string::npos)
        << "got: " << it->message;
}

// ── Direct OperatorTable construction ─────────────────────────────────────
//
// The loader is the only writer in production. These tests exercise the
// class API directly so semantics like overwrite, move, and the empty
// invariant are pinned independent of any loader path.

TEST(OperatorTable, DirectInsertAndLookupRoundTrip) {
    OperatorTable t;
    SchemaTokenId const id{42};
    t.insert(id, OperatorArity::Infix, {50, OperatorAssoc::Left});
    auto e = t.lookup(id, OperatorArity::Infix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->precedence, 50);
    EXPECT_EQ(e->associativity, OperatorAssoc::Left);
}

TEST(OperatorTable, DirectInsertOverwritesLastWriteWins) {
    OperatorTable t;
    SchemaTokenId const id{7};
    t.insert(id, OperatorArity::Infix, {10, OperatorAssoc::Left});
    t.insert(id, OperatorArity::Infix, {99, OperatorAssoc::Right});
    auto e = t.lookup(id, OperatorArity::Infix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->precedence, 99);
    EXPECT_EQ(e->associativity, OperatorAssoc::Right);
    EXPECT_EQ(t.size(), 1u);
}

TEST(OperatorTable, EmptyAndSizeReflectInsertions) {
    OperatorTable t;
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(t.size(), 0u);
    t.insert(SchemaTokenId{1}, OperatorArity::Infix,  {1, OperatorAssoc::Left});
    t.insert(SchemaTokenId{1}, OperatorArity::Prefix, {2, OperatorAssoc::Right});
    EXPECT_FALSE(t.empty());
    EXPECT_EQ(t.size(), 2u);
}

TEST(OperatorTable, CopyIsIndependent) {
    OperatorTable a;
    a.insert(SchemaTokenId{3}, OperatorArity::Infix, {30, OperatorAssoc::Left});
    OperatorTable b = a;
    b.insert(SchemaTokenId{3}, OperatorArity::Infix, {31, OperatorAssoc::Right});
    auto fromA = a.lookup(SchemaTokenId{3}, OperatorArity::Infix);
    auto fromB = b.lookup(SchemaTokenId{3}, OperatorArity::Infix);
    ASSERT_TRUE(fromA.has_value());
    ASSERT_TRUE(fromB.has_value());
    EXPECT_EQ(fromA->precedence, 30);
    EXPECT_EQ(fromB->precedence, 31);
}

TEST(OperatorTable, MoveTransfersOwnership) {
    OperatorTable a;
    a.insert(SchemaTokenId{5}, OperatorArity::Prefix, {70, OperatorAssoc::Right});
    OperatorTable b = std::move(a);
    auto e = b.lookup(SchemaTokenId{5}, OperatorArity::Prefix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->precedence, 70);
}

// The 30-bit ceiling on SchemaTokenId.v is exposed as
// `OperatorTable::kMaxSchemaTokenIdValue`. Inserting at exactly the
// ceiling must round-trip — proves key() handles the boundary correctly.
TEST(OperatorTable, KeyPackingHandlesCeilingValue) {
    OperatorTable t;
    SchemaTokenId const id{OperatorTable::kMaxSchemaTokenIdValue};
    t.insert(id, OperatorArity::Postfix, {80, OperatorAssoc::Left});
    auto e = t.lookup(id, OperatorArity::Postfix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->precedence, 80);
    // Other arities at the same id must be definitively empty — proves
    // the arity bits didn't get clobbered by the high bits of v.
    EXPECT_FALSE(t.lookup(id, OperatorArity::Infix).has_value());
    EXPECT_FALSE(t.lookup(id, OperatorArity::Prefix).has_value());
}

// Aborts when SchemaTokenId.v exceeds the packing budget. Pairs with
// opTableFatal — the message regex is the fatal helper's signature.
TEST(OperatorTableDeath, KeyPackingAbortsOnCeilingOverflow) {
    OperatorTable t;
    EXPECT_DEATH({
        t.insert(SchemaTokenId{OperatorTable::kMaxSchemaTokenIdValue + 1u},
                 OperatorArity::Infix, {0, OperatorAssoc::None});
    }, "exceeds 30-bit key budget");
}
