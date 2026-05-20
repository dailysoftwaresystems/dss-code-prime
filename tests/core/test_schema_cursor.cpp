#include "core/types/compiled_shape.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/schema_cursor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

using namespace dss;

namespace {

[[nodiscard]] std::shared_ptr<GrammarSchema const> load(std::string_view text) {
    auto loaded = GrammarSchema::loadFromText(text);
    EXPECT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    return loaded.has_value() ? *loaded : nullptr;
}

[[nodiscard]] bool firstSetContains(std::span<SchemaTokenId const> s, SchemaTokenId t) {
    return std::ranges::find_if(s, [&](SchemaTokenId x) { return x.v == t.v; }) != s.end();
}

[[nodiscard]] SchemaTokenId kindId(GrammarSchema const& s, std::string_view name) {
    return s.schemaTokens().find(name);
}

[[nodiscard]] RuleId ruleId(GrammarSchema const& s, std::string_view name) {
    return s.rules().find(name);
}

} // namespace

// ── Sequence advancement ──────────────────────────────────────────────────

TEST(SchemaCursor, RootCursorPointsAtFirstRuleStep) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp", "Identifier" ] } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    auto root = schema->rootCursor();
    ASSERT_TRUE(root.valid());
    EXPECT_EQ(schema->slotKind(root), SlotKind::TokenLeaf);

    auto expected = schema->expectedSet(root);
    EXPECT_TRUE(firstSetContains(expected, kindId(*schema, "PlusOp")));
    EXPECT_FALSE(firstSetContains(expected, kindId(*schema, "Identifier")));
}

TEST(SchemaCursor, AdvanceThroughSequenceStepByStep) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        ";": [{ "kind": "End" }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp", "Identifier", "End" ] } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    auto c0 = schema->rootCursor();
    auto c1 = schema->advance(c0, kindId(*schema, "PlusOp"));
    ASSERT_TRUE(c1.valid());

    auto exp1 = schema->expectedSet(c1);
    EXPECT_TRUE(firstSetContains(exp1, kindId(*schema, "Identifier")));
    EXPECT_FALSE(firstSetContains(exp1, kindId(*schema, "End")));

    auto c2 = schema->advance(c1, kindId(*schema, "Identifier"));
    ASSERT_TRUE(c2.valid());
    auto exp2 = schema->expectedSet(c2);
    EXPECT_TRUE(firstSetContains(exp2, kindId(*schema, "End")));

    auto c3 = schema->advance(c2, kindId(*schema, "End"));
    ASSERT_TRUE(c3.valid());
    EXPECT_TRUE(schema->isAtEndOfRule(c3));
    EXPECT_TRUE(schema->canEndSource(c3));
    EXPECT_FALSE(schema->canEndSource(c0));   // not yet
}

TEST(SchemaCursor, AdvanceWithWrongTokenReturnsInvalidCursor) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        "-": [{ "kind": "MinusOp" }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);
    auto bad = schema->advance(schema->rootCursor(), kindId(*schema, "MinusOp"));
    EXPECT_FALSE(bad.valid());
}

// ── Alt branching ─────────────────────────────────────────────────────────

TEST(SchemaCursor, AltExpectedSetIsUnionOfBranchFirsts) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        "-": [{ "kind": "MinusOp" }],
        "*": [{ "kind": "StarOp" }]
      },
      "shapes": { "root": { "alt": [ "PlusOp", "MinusOp", "StarOp" ] } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    auto root = schema->rootCursor();
    EXPECT_EQ(schema->slotKind(root), SlotKind::AltChoice);
    auto exp = schema->expectedSet(root);
    EXPECT_TRUE(firstSetContains(exp, kindId(*schema, "PlusOp")));
    EXPECT_TRUE(firstSetContains(exp, kindId(*schema, "MinusOp")));
    EXPECT_TRUE(firstSetContains(exp, kindId(*schema, "StarOp")));
}

TEST(SchemaCursor, AltAdvanceRoutesIntoMatchingBranch) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        "-": [{ "kind": "MinusOp" }]
      },
      "shapes": { "root": { "alt": [ "PlusOp", "MinusOp" ] } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    auto c = schema->advance(schema->rootCursor(), kindId(*schema, "MinusOp"));
    ASSERT_TRUE(c.valid());
    EXPECT_TRUE(schema->isAtEndOfRule(c));
}

// ── Optional ─────────────────────────────────────────────────────────────

TEST(SchemaCursor, OptionalExpectedSetIncludesInnerAndContinuation) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        ";": [{ "kind": "End" }]
      },
      "shapes": {
        "root": { "sequence": [ { "optional": "PlusOp" }, "End" ] }
      }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    auto c0 = schema->rootCursor();
    auto exp = schema->expectedSet(c0);
    EXPECT_TRUE(firstSetContains(exp, kindId(*schema, "PlusOp")));
    EXPECT_TRUE(firstSetContains(exp, kindId(*schema, "End")));

    auto withPlus = schema->advance(c0, kindId(*schema, "PlusOp"));
    ASSERT_TRUE(withPlus.valid());
    auto cEnd1 = schema->advance(withPlus, kindId(*schema, "End"));
    ASSERT_TRUE(cEnd1.valid());
    EXPECT_TRUE(schema->canEndSource(cEnd1));

    auto cEnd2 = schema->advance(c0, kindId(*schema, "End"));
    ASSERT_TRUE(cEnd2.valid());
    EXPECT_TRUE(schema->canEndSource(cEnd2));
}

TEST(SchemaCursor, RuleWithOnlyOptionalBodyIsNullable) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "optional": "PlusOp" } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    EXPECT_TRUE(schema->isNullable(ruleId(*schema, "root")));
    EXPECT_TRUE(schema->canEndSource(schema->rootCursor()));
}

// ── Repeat ───────────────────────────────────────────────────────────────

TEST(SchemaCursor, RepeatLoopsBackToEntryAfterEachIteration) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        ";": [{ "kind": "End" }]
      },
      "shapes": {
        "root": { "sequence": [ { "repeat": "PlusOp" }, "End" ] }
      }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    auto loopEntry = schema->rootCursor();
    auto exp0 = schema->expectedSet(loopEntry);
    EXPECT_TRUE(firstSetContains(exp0, kindId(*schema, "PlusOp")));
    EXPECT_TRUE(firstSetContains(exp0, kindId(*schema, "End")));

    auto after1 = schema->advance(loopEntry, kindId(*schema, "PlusOp"));
    ASSERT_TRUE(after1.valid());
    auto after2 = schema->advance(after1, kindId(*schema, "PlusOp"));
    ASSERT_TRUE(after2.valid());
    // After each iteration the cursor must land back at the loop entry —
    // pointer-identity on expectedSet's underlying storage proves the
    // back-edge points at the same position, not at a structural twin.
    EXPECT_EQ(schema->expectedSet(after1).data(), exp0.data());
    EXPECT_EQ(after1.posId(), loopEntry.posId());
    EXPECT_EQ(schema->expectedSet(after2).data(), exp0.data());
    EXPECT_EQ(after2.posId(), loopEntry.posId());

    auto cEnd = schema->advance(after2, kindId(*schema, "End"));
    ASSERT_TRUE(cEnd.valid());
    EXPECT_TRUE(schema->canEndSource(cEnd));

    auto cEmptyExit = schema->advance(loopEntry, kindId(*schema, "End"));
    ASSERT_TRUE(cEmptyExit.valid());
    EXPECT_TRUE(schema->canEndSource(cEmptyExit));
}

// ── enterRule / leaveRule (caller-managed descent stack) ─────────────────

TEST(SchemaCursor, EnterRuleReturnsCursorAtChildStart) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        ";": [{ "kind": "End" }]
      },
      "shapes": {
        "root":  { "sequence": [ "inner", "End" ] },
        "inner": { "sequence": [ "PlusOp" ] }
      }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    auto root = schema->rootCursor();
    EXPECT_EQ(schema->slotKind(root), SlotKind::RuleLeaf);
    EXPECT_EQ(schema->slotRuleRef(root).v, ruleId(*schema, "inner").v);

    auto innerStart = schema->enterRule(ruleId(*schema, "inner"));
    ASSERT_TRUE(innerStart.valid());
    EXPECT_EQ(innerStart.rule().v, ruleId(*schema, "inner").v);

    auto exp = schema->expectedSet(innerStart);
    EXPECT_TRUE(firstSetContains(exp, kindId(*schema, "PlusOp")));
    EXPECT_FALSE(firstSetContains(exp, kindId(*schema, "End")));
}

TEST(SchemaCursor, LeaveRuleAdvancesParentPastTheSlot) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        ";": [{ "kind": "End" }]
      },
      "shapes": {
        "root":  { "sequence": [ "inner", "End" ] },
        "inner": { "sequence": [ "PlusOp" ] }
      }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    auto parent = schema->rootCursor();
    auto resumed = schema->leaveRule(parent);
    ASSERT_TRUE(resumed.valid());
    auto exp = schema->expectedSet(resumed);
    EXPECT_TRUE(firstSetContains(exp, kindId(*schema, "End")));
    EXPECT_FALSE(firstSetContains(exp, kindId(*schema, "PlusOp")));
}

TEST(SchemaCursor, LeaveRuleOnNonRuleSlotIsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);
    // Cursor sits on a TokenLeaf; leaveRule must return an invalid cursor
    // rather than corrupting state.
    auto bogus = schema->leaveRule(schema->rootCursor());
    EXPECT_FALSE(bogus.valid());
}

// ── Cycle handling ────────────────────────────────────────────────────────

TEST(SchemaCursor, CycleThroughRepeatTerminates) {
    // a -> repeat[b]; b -> sequence[Identifier, a]. The cycle goes through
    // a non-nullable token (Identifier) so FIRST sets converge in finite
    // iterations and the cursor walk is well-formed. The load itself
    // would hang or stack-overflow if the fixed-point loop didn't
    // terminate.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": {
        "root": { "sequence": [ "a", "PlusOp" ] },
        "a":    { "repeat":   "b" },
        "b":    { "sequence": [ "Identifier", "a" ] }
      }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    auto firstA = schema->firstSetOf(ruleId(*schema, "a"));
    auto firstB = schema->firstSetOf(ruleId(*schema, "b"));
    EXPECT_TRUE(schema->isNullable(ruleId(*schema, "a")));   // repeat → nullable
    EXPECT_FALSE(schema->isNullable(ruleId(*schema, "b")));  // starts with Identifier
    EXPECT_TRUE(firstSetContains(firstB, kindId(*schema, "Identifier")));
    EXPECT_TRUE(firstSetContains(firstA, kindId(*schema, "Identifier")));

    // expectedSet at a RuleLeaf is FIRST(rule) only — the caller decides
    // whether to descend (enterRule) or skip past a nullable rule
    // (leaveRule + check isNullable). Skipping is NOT auto-folded into
    // expectedSet; the API stays primitive.
    auto exp = schema->expectedSet(schema->rootCursor());
    EXPECT_TRUE(firstSetContains(exp, kindId(*schema, "Identifier")));
    EXPECT_FALSE(firstSetContains(exp, kindId(*schema, "PlusOp")));

    // Skipping the nullable `a` and then matching PlusOp must work via
    // explicit leaveRule.
    auto afterA = schema->leaveRule(schema->rootCursor());
    ASSERT_TRUE(afterA.valid());
    auto cEnd = schema->advance(afterA, kindId(*schema, "PlusOp"));
    ASSERT_TRUE(cEnd.valid());
    EXPECT_TRUE(schema->canEndSource(cEnd));
}

// ── canEndSource ──────────────────────────────────────────────────────────

TEST(SchemaCursor, CanEndSourceOnlyAtRootEnd) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": {
        "root":  { "sequence": [ "inner" ] },
        "inner": { "sequence": [ "PlusOp" ] }
      }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    EXPECT_FALSE(schema->canEndSource(schema->rootCursor()));
    auto rootEnd = schema->leaveRule(schema->rootCursor());
    EXPECT_TRUE(schema->canEndSource(rootEnd));

    // canEndSource only fires in the root rule. End-of-body inside a
    // non-root rule must not be a "source can end here" signal.
    auto innerEnd = schema->advance(
        schema->enterRule(ruleId(*schema, "inner")),
        kindId(*schema, "PlusOp"));
    ASSERT_TRUE(innerEnd.valid());
    EXPECT_TRUE(schema->isAtEndOfRule(innerEnd));
    EXPECT_FALSE(schema->canEndSource(innerEnd));
}

// ── Direct FIRST / nullable queries ───────────────────────────────────────

TEST(SchemaCursor, FirstSetOfTokenOnlyRuleIsThatToken) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);
    auto fs = schema->firstSetOf(ruleId(*schema, "root"));
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].v, kindId(*schema, "PlusOp").v);
}

TEST(SchemaCursor, FirstSetOfInvalidRuleIsEmpty) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->firstSetOf(InvalidRule).size(), 0u);
    EXPECT_FALSE(schema->isNullable(InvalidRule));
}

// ── expr shape kind ───────────────────────────────────────────────────────

TEST(SchemaCursor, ExprShapeBehavesAsAtomReferenceForCursor) {
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
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    auto fs = schema->firstSetOf(ruleId(*schema, "expression"));
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_TRUE(firstSetContains(fs, kindId(*schema, "Identifier")));
}

// ── Default-constructed cursor ────────────────────────────────────────────

TEST(SchemaCursor, DefaultConstructedCursorIsInvalid) {
    SchemaCursor c;
    EXPECT_FALSE(c.valid());
}

// ── All-nullable cycle: FIRST stays empty, fixed-point still converges ──

TEST(SchemaCursor, AllNullableCycleTerminates) {
    // a → repeat[optional[b]] is nullable from every step (both `repeat`
    // and `optional` are nullable). b → optional[a] is nullable too. The
    // grammar has no token references at all, so FIRST stays empty
    // throughout. Fixed-point termination relies on `(firstSet, nullable)`
    // reaching a stable pair, NOT on FIRST growing. If a future refactor
    // ever made convergence depend on FIRST monotonicity alone, this test
    // would hang during load.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": {
        "root": { "sequence": [ "a", "PlusOp" ] },
        "a":    { "repeat":   "b" },
        "b":    { "optional": "a" }
      }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);
    EXPECT_TRUE(schema->isNullable(ruleId(*schema, "a")));
    EXPECT_TRUE(schema->isNullable(ruleId(*schema, "b")));
    EXPECT_EQ(schema->firstSetOf(ruleId(*schema, "a")).size(), 0u);
    EXPECT_EQ(schema->firstSetOf(ruleId(*schema, "b")).size(), 0u);
}

// ── AltChoice ambiguity is a load error ──────────────────────────────────

TEST(SchemaCursor, AltBranchesShareFirstTokenIsLoadError) {
    // Two branches whose FIRST sets overlap on `PlusOp` — the cursor would
    // silently first-branch-win on advance. The loader rejects this so
    // the config author either restructures the grammar or factors the
    // shared prefix into a parent rule.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        "*": [{ "kind": "StarOp" }]
      },
      "shapes": {
        "root": {
          "alt": [
            { "sequence": [ "PlusOp", "Identifier" ] },
            { "sequence": [ "PlusOp", "StarOp" ] }
          ]
        }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    auto const& diags = loaded.error();
    auto it = std::ranges::find_if(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_AmbiguousAlternatives;
    });
    ASSERT_NE(it, diags.end());
    EXPECT_NE(it->message.find("PlusOp"), std::string::npos)
        << "diagnostic should name the offending token; got: " << it->message;
}

TEST(SchemaCursor, NonAmbiguousAltLoadsCleanly) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        "*": [{ "kind": "StarOp" }]
      },
      "shapes": { "root": { "alt": [ "PlusOp", "StarOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
}

// ── Multi-level descent: enterRule × leaveRule × leaveRule ───────────────

TEST(SchemaCursor, MultiLevelDescentAndReturn) {
    // root → outer → inner → tok. Caller manages a 3-deep cursor stack.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": {
        "root":  { "sequence": [ "outer", "PlusOp" ] },
        "outer": { "sequence": [ "inner" ] },
        "inner": { "sequence": [ "Identifier" ] }
      }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    // Save root, descend into outer.
    auto rootCur  = schema->rootCursor();
    EXPECT_EQ(schema->slotRuleRef(rootCur).v, ruleId(*schema, "outer").v);
    auto outerCur = schema->enterRule(ruleId(*schema, "outer"));
    ASSERT_TRUE(outerCur.valid());

    // Save outer, descend into inner.
    EXPECT_EQ(schema->slotRuleRef(outerCur).v, ruleId(*schema, "inner").v);
    auto innerCur = schema->enterRule(ruleId(*schema, "inner"));
    ASSERT_TRUE(innerCur.valid());

    // Match Identifier inside inner.
    auto innerEnd = schema->advance(innerCur, kindId(*schema, "Identifier"));
    ASSERT_TRUE(innerEnd.valid());
    EXPECT_TRUE(schema->isAtEndOfRule(innerEnd));

    // Pop back to outer (advance past the inner ruleref), then to root.
    auto outerAfter = schema->leaveRule(outerCur);
    ASSERT_TRUE(outerAfter.valid());
    EXPECT_TRUE(schema->isAtEndOfRule(outerAfter));

    auto rootAfter = schema->leaveRule(rootCur);
    ASSERT_TRUE(rootAfter.valid());
    // Now expecting PlusOp.
    EXPECT_TRUE(firstSetContains(schema->expectedSet(rootAfter),
                                  kindId(*schema, "PlusOp")));
    auto rootEnd = schema->advance(rootAfter, kindId(*schema, "PlusOp"));
    ASSERT_TRUE(rootEnd.valid());
    EXPECT_TRUE(schema->canEndSource(rootEnd));
}

// ── advance on RuleLeaf is an invalid no-op ──────────────────────────────

TEST(SchemaCursor, AdvanceOnRuleLeafReturnsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": {
        "root":  { "sequence": [ "inner" ] },
        "inner": { "sequence": [ "PlusOp" ] }
      }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    auto cur = schema->rootCursor();
    EXPECT_EQ(schema->slotKind(cur), SlotKind::RuleLeaf);
    // The token IS in FIRST(inner), but caller must enterRule first.
    // advance refuses rather than silently descending.
    auto bogus = schema->advance(cur, kindId(*schema, "PlusOp"));
    EXPECT_FALSE(bogus.valid());
}

// ── expectedSet ordering and span stability ──────────────────────────────

TEST(SchemaCursor, ExpectedSetOrderedBySchemaTokenIdValue) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "a": [{ "kind": "Alpha" }],
        "b": [{ "kind": "Beta"  }],
        "c": [{ "kind": "Gamma" }]
      },
      "shapes": { "root": { "alt": [ "Gamma", "Alpha", "Beta" ] } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    auto exp = schema->expectedSet(schema->rootCursor());
    ASSERT_EQ(exp.size(), 3u);
    // mergeSorted produces ids in ascending .v order regardless of the
    // alt declaration order. Pinning ascending order locks the contract
    // future consumers may use for binary search.
    for (std::size_t i = 1; i < exp.size(); ++i) {
        EXPECT_LT(exp[i - 1].v, exp[i].v);
    }
}

TEST(SchemaCursor, ExpectedSetSpanIsStableAcrossCursorOperations) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ { "repeat": "PlusOp" }, "Identifier" ] } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    // Loop-entry position; expectedSet built via tie-the-knot. Capture
    // pointer+size, run a sequence of cursor operations, re-check that
    // the original span still points at the same storage.
    auto loopEntry = schema->rootCursor();
    auto exp0      = schema->expectedSet(loopEntry);
    auto* data0    = exp0.data();
    auto  size0    = exp0.size();

    auto after  = schema->advance(loopEntry, kindId(*schema, "PlusOp"));
    auto after2 = schema->advance(after,     kindId(*schema, "PlusOp"));
    auto cEnd   = schema->advance(after2,    kindId(*schema, "Identifier"));
    (void)cEnd;

    auto expAgain = schema->expectedSet(loopEntry);
    EXPECT_EQ(expAgain.data(), data0);
    EXPECT_EQ(expAgain.size(), size0);
}

TEST(SchemaCursor, FirstSetOfReturnsStableSpan) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);
    auto a = schema->firstSetOf(ruleId(*schema, "root"));
    auto b = schema->firstSetOf(ruleId(*schema, "root"));
    EXPECT_EQ(a.data(), b.data());
    EXPECT_EQ(a.size(), b.size());
}

// ── enterRule on invalid / non-existent rules ────────────────────────────

TEST(SchemaCursor, EnterRuleOnInvalidRuleReturnsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);
    EXPECT_FALSE(schema->enterRule(InvalidRule).valid());
    EXPECT_FALSE(schema->enterRule(RuleId{9999}).valid());
}

TEST(SchemaCursor, OperationsOnInvalidCursorReturnInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto schema = load(kCfg);
    ASSERT_NE(schema, nullptr);

    SchemaCursor invalid;
    EXPECT_FALSE(schema->advance(invalid, kindId(*schema, "PlusOp")).valid());
    EXPECT_FALSE(schema->leaveRule(invalid).valid());
    EXPECT_TRUE(schema->expectedSet(invalid).empty());
    EXPECT_EQ(schema->slotKind(invalid), SlotKind::End);
    EXPECT_FALSE(schema->canEndSource(invalid));
}

// ── routeToRuleLeaf — multi-level nested AltChoice (SH4c) ─────────────────
//
// v2-gap-catalog §7 row 21 flagged `routeToRuleLeaf` as "untested past two
// levels of nested AltChoice." PR2b shipped the routing logic but the only
// in-tree exercise was the 2-level alt-of-rules case. The tests below pin
// 3-level and 4-level nesting end-to-end so the parser phase doesn't
// rediscover a routing bug under live grammar pressure.
//
// AltChoice slots arise from three shape kinds: `alt`, `optional` (the
// take-or-skip branch), and `repeat` (the iterate-or-exit branch). Nesting
// these composes AltChoice positions; the recursion in routeToRuleLeaf
// should descend through every level.

namespace {

// `root → alt( optional( alt( funcDecl | varDecl ) ) | typedefDecl )`
//
// Levels of AltChoice between the root cursor and `funcDecl`'s RuleLeaf:
//   1) outer `alt(... | typedefDecl)`
//   2) the optional's take-or-skip
//   3) inner `alt(funcDecl | varDecl)`
//
// Three AltChoice slots stack between the entry cursor and the RuleLeaf —
// strictly past the "two levels" the v2 review covered.
constexpr std::string_view kThreeLevelCfg = R"JSON({
  "dssSchemaVersion": 1,
  "language": { "name": "Nested3", "version": "0.1.0" },
  "tokens": {
    "f": [{ "kind": "FKeyword" }],
    "v": [{ "kind": "VKeyword" }],
    "t": [{ "kind": "TKeyword" }]
  },
  "shapes": {
    "root": {
      "alt": [
        { "optional": {
            "alt": [ "funcDecl", "varDecl" ]
          }
        },
        "typedefDecl"
      ]
    },
    "funcDecl":    { "sequence": [ "FKeyword" ] },
    "varDecl":     { "sequence": [ "VKeyword" ] },
    "typedefDecl": { "sequence": [ "TKeyword" ] }
  }
})JSON";

// Add another optional wrapper to take it to four levels:
//   alt → optional → alt → optional → RuleLeaf(funcDecl)
constexpr std::string_view kFourLevelCfg = R"JSON({
  "dssSchemaVersion": 1,
  "language": { "name": "Nested4", "version": "0.1.0" },
  "tokens": {
    "f": [{ "kind": "FKeyword" }],
    "v": [{ "kind": "VKeyword" }],
    "t": [{ "kind": "TKeyword" }]
  },
  "shapes": {
    "root": {
      "alt": [
        { "optional": {
            "alt": [
              { "optional": "funcDecl" },
              "varDecl"
            ]
          }
        },
        "typedefDecl"
      ]
    },
    "funcDecl":    { "sequence": [ "FKeyword" ] },
    "varDecl":     { "sequence": [ "VKeyword" ] },
    "typedefDecl": { "sequence": [ "TKeyword" ] }
  }
})JSON";

} // namespace

TEST(SchemaCursorRouteRuleLeaf, ThreeLevelNestingRoutesToFuncDecl) {
    auto schema = load(kThreeLevelCfg);
    ASSERT_NE(schema, nullptr);

    auto root = schema->rootCursor();
    ASSERT_TRUE(root.valid());
    ASSERT_EQ(schema->slotKind(root), SlotKind::AltChoice);

    const auto funcRule = ruleId(*schema, "funcDecl");
    ASSERT_TRUE(funcRule.valid());

    auto routed = schema->routeToRuleLeaf(root, funcRule);
    ASSERT_TRUE(routed.valid());
    EXPECT_EQ(schema->slotKind(routed), SlotKind::RuleLeaf);

    // leaveRule on the routed RuleLeaf must produce a valid post-rule
    // cursor — that's the invariant the v2 review found broken when a
    // non-RuleLeaf was saved.
    auto after = schema->leaveRule(routed);
    EXPECT_TRUE(after.valid() || schema->slotKind(after) == SlotKind::End);
}

TEST(SchemaCursorRouteRuleLeaf, ThreeLevelNestingRoutesToVarDecl) {
    auto schema = load(kThreeLevelCfg);
    ASSERT_NE(schema, nullptr);

    auto root = schema->rootCursor();
    const auto varRule = ruleId(*schema, "varDecl");
    ASSERT_TRUE(varRule.valid());

    auto routed = schema->routeToRuleLeaf(root, varRule);
    ASSERT_TRUE(routed.valid());
    EXPECT_EQ(schema->slotKind(routed), SlotKind::RuleLeaf);
}

TEST(SchemaCursorRouteRuleLeaf, ThreeLevelNestingRoutesToOuterSibling) {
    // typedefDecl sits at the outer-alt level only — one AltChoice deep,
    // not three. Routing must still find it without descending into the
    // optional's sub-tree.
    auto schema = load(kThreeLevelCfg);
    ASSERT_NE(schema, nullptr);

    auto root = schema->rootCursor();
    const auto typedefRule = ruleId(*schema, "typedefDecl");
    ASSERT_TRUE(typedefRule.valid());

    auto routed = schema->routeToRuleLeaf(root, typedefRule);
    ASSERT_TRUE(routed.valid());
    EXPECT_EQ(schema->slotKind(routed), SlotKind::RuleLeaf);
}

TEST(SchemaCursorRouteRuleLeaf, ThreeLevelNestingReturnsInvalidForUnreachableRule) {
    auto schema = load(kThreeLevelCfg);
    ASSERT_NE(schema, nullptr);

    auto root = schema->rootCursor();
    // Use a rule that isn't reachable from root's AltChoice tree — `root`
    // itself is the parent, recursion through AltChoice can't surface it
    // as a RuleLeaf branch.
    const auto rootRule = ruleId(*schema, "root");
    ASSERT_TRUE(rootRule.valid());

    auto routed = schema->routeToRuleLeaf(root, rootRule);
    EXPECT_FALSE(routed.valid());
}

TEST(SchemaCursorRouteRuleLeaf, FourLevelNestingRoutesToFuncDecl) {
    // The deepest target — `funcDecl` sits inside an optional inside an
    // alt inside an optional inside the outer alt. Four AltChoice slots
    // between root and the RuleLeaf.
    auto schema = load(kFourLevelCfg);
    ASSERT_NE(schema, nullptr);

    auto root = schema->rootCursor();
    ASSERT_EQ(schema->slotKind(root), SlotKind::AltChoice);

    const auto funcRule = ruleId(*schema, "funcDecl");
    ASSERT_TRUE(funcRule.valid());

    auto routed = schema->routeToRuleLeaf(root, funcRule);
    ASSERT_TRUE(routed.valid());
    EXPECT_EQ(schema->slotKind(routed), SlotKind::RuleLeaf);

    auto after = schema->leaveRule(routed);
    EXPECT_TRUE(after.valid() || schema->slotKind(after) == SlotKind::End);
}

TEST(SchemaCursorRouteRuleLeaf, FourLevelNestingRoutesToVarDecl) {
    auto schema = load(kFourLevelCfg);
    ASSERT_NE(schema, nullptr);

    auto root = schema->rootCursor();
    const auto varRule = ruleId(*schema, "varDecl");
    ASSERT_TRUE(varRule.valid());

    auto routed = schema->routeToRuleLeaf(root, varRule);
    ASSERT_TRUE(routed.valid());
    EXPECT_EQ(schema->slotKind(routed), SlotKind::RuleLeaf);
}

TEST(SchemaCursorRouteRuleLeaf, AlreadyAtRuleLeafIsIdentity) {
    // If the caller hands routeToRuleLeaf a cursor that is already the
    // RuleLeaf for the requested rule, the routing returns it unchanged.
    // Documents the entry path the recursion's base case takes.
    auto schema = load(kThreeLevelCfg);
    ASSERT_NE(schema, nullptr);

    const auto funcRule = ruleId(*schema, "funcDecl");
    auto funcEntry = schema->enterRule(funcRule);
    ASSERT_TRUE(funcEntry.valid());

    // funcDecl's body is a TokenLeaf, not a RuleLeaf — confirm the
    // identity branch fires on a synthesized RuleLeaf cursor. Walk root's
    // AltChoice path until we land on a RuleLeaf, then re-route through
    // that exact cursor.
    auto root = schema->rootCursor();
    auto routed = schema->routeToRuleLeaf(root, funcRule);
    ASSERT_TRUE(routed.valid());
    EXPECT_EQ(schema->slotKind(routed), SlotKind::RuleLeaf);

    // Second pass on the routed cursor — must be identity (no further descent
    // possible — we're already at the RuleLeaf for `funcRule`).
    auto re_routed = schema->routeToRuleLeaf(routed, funcRule);
    EXPECT_EQ(re_routed.rule().v, routed.rule().v);
}
