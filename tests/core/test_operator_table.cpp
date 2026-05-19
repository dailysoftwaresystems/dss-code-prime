#include "core/types/grammar_schema.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

using namespace dss;

namespace {

// Minimum-viable config exercising every operator-table feature:
//   `+` and `-` are infix with left-associativity at the same precedence
//   `*` is infix left-assoc at a higher precedence
//   `-` is ALSO prefix at the highest precedence (distinct arity slot)
//   `=` is right-associative
//   `!` is prefix
//   `++` is postfix
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
      { "precedence": 80, "associativity": "left",  "arity": "postfix",  "operators": ["++"]      }
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

// PR1 acceptance bullet (a): infix lookup returns the configured precedence.
TEST(OperatorTable, InfixPlusReturnsPrecedence50) {
    auto schema = loadOpsConfig();
    ASSERT_NE(schema, nullptr);
    auto e = schema->operatorTable().lookup(kindId(*schema, "PlusOp"),
                                            OperatorArity::Infix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->precedence, 50);
    EXPECT_EQ(e->associativity, OperatorAssoc::Left);
}

// PR1 acceptance bullet (b): associativity round-trips.
TEST(OperatorTable, AssignIsRightAssoc) {
    auto schema = loadOpsConfig();
    ASSERT_NE(schema, nullptr);
    auto e = schema->operatorTable().lookup(kindId(*schema, "AssignOp"),
                                            OperatorArity::Infix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->precedence, 10);
    EXPECT_EQ(e->associativity, OperatorAssoc::Right);
}

// PR1 acceptance bullet (c): same lexeme can be both prefix AND infix with
// distinct entries. `-` as prefix is precedence 70; as infix it is 50.
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

// PR1 acceptance bullet (d): unrelated kinds (Identifier here) return none.
TEST(OperatorTable, IdentifierIsNotAnOperator) {
    auto schema = loadOpsConfig();
    ASSERT_NE(schema, nullptr);
    EXPECT_FALSE(schema->operatorTable()
                     .lookup(kindId(*schema, "Identifier"), OperatorArity::Infix).has_value());
}

// Postfix arity carries through and is independent from infix/prefix.
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

// Default arity for groups without an explicit `arity` is Infix.
TEST(OperatorTable, DefaultArityIsInfix) {
    auto schema = loadOpsConfig();
    ASSERT_NE(schema, nullptr);
    // `*` group has no `arity` field — must land in the Infix slot.
    auto e = schema->operatorTable().lookup(kindId(*schema, "StarOp"),
                                            OperatorArity::Infix);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->precedence, 60);
}

// Default associativity for groups without an explicit `associativity` is None.
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

// A multi-meaning lexeme without an explicit `kind` on the group is a load
// error: the loader can't decide which of the meanings should carry the
// operator metadata.
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
}

// With an explicit `kind` on the group, the loader stamps the precedence
// onto the chosen meaning and ignores the other(s).
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

// Explicit `kind` that names a token kind which exists but is NOT a
// meaning of the named lexeme is rejected.
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

// Unknown lexeme in an operators array is rejected.
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

// Bad associativity / arity strings are rejected with C_InvalidPrecedenceTable.
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

// Backwards compat: a v1 config with no `operators` section yields an
// empty operatorTable. toy.lang.json must remain in this state.
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

// `expr` shape kind round-trip: a rule body using `{ "expr": { "atom": "primary" } }`
// loads cleanly and the atom rule reference is validated. A typo in `atom`
// surfaces as C_UnknownShape.
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
