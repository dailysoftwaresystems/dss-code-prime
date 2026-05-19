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
    EXPECT_EQ(schema->slotKind(root), detail::SlotKind::TokenLeaf);

    // Position 0 of `root` expects PlusOp; not Identifier yet.
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

    // Now at step 1; expects Identifier next.
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
    EXPECT_EQ(schema->slotKind(root), detail::SlotKind::AltChoice);
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
    // Both the optional's inner FIRST and the continuation's FIRST must be valid.
    EXPECT_TRUE(firstSetContains(exp, kindId(*schema, "PlusOp")));
    EXPECT_TRUE(firstSetContains(exp, kindId(*schema, "End")));

    // Take the optional.
    auto withPlus = schema->advance(c0, kindId(*schema, "PlusOp"));
    ASSERT_TRUE(withPlus.valid());
    auto cEnd1 = schema->advance(withPlus, kindId(*schema, "End"));
    ASSERT_TRUE(cEnd1.valid());
    EXPECT_TRUE(schema->canEndSource(cEnd1));

    // Skip the optional.
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

    // Match PlusOp twice; cursor must return to the same loop entry.
    auto after1 = schema->advance(loopEntry, kindId(*schema, "PlusOp"));
    ASSERT_TRUE(after1.valid());
    auto after2 = schema->advance(after1, kindId(*schema, "PlusOp"));
    ASSERT_TRUE(after2.valid());
    // Both intermediate cursors share the loop-entry's expectedSet — they
    // ARE the loop entry, returned to after each iteration of `repeat`.
    EXPECT_EQ(schema->expectedSet(after1).data(), exp0.data());
    EXPECT_EQ(schema->expectedSet(after2).data(), exp0.data());

    // Exit the loop.
    auto cEnd = schema->advance(after2, kindId(*schema, "End"));
    ASSERT_TRUE(cEnd.valid());
    EXPECT_TRUE(schema->canEndSource(cEnd));

    // Empty-loop exit is also legal.
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
    // root's first slot is a RuleLeaf for `inner`.
    EXPECT_EQ(schema->slotKind(root), detail::SlotKind::RuleLeaf);
    EXPECT_EQ(schema->slotRuleRef(root).v, ruleId(*schema, "inner").v);

    auto innerStart = schema->enterRule(ruleId(*schema, "inner"));
    ASSERT_TRUE(innerStart.valid());
    EXPECT_EQ(innerStart.rule().v, ruleId(*schema, "inner").v);

    // Token-level expectedSet inside `inner` is {PlusOp}.
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
    // (caller would save `parent`, descend, ...)
    auto resumed = schema->leaveRule(parent);
    ASSERT_TRUE(resumed.valid());
    // Now at parent's step-after-inner, expecting End.
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

    // The same end-of-body cursor inside a NON-root rule is not source-end.
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

    // FIRST(expression) = FIRST(primary) = {Identifier}.
    auto fs = schema->firstSetOf(ruleId(*schema, "expression"));
    EXPECT_TRUE(firstSetContains(fs, kindId(*schema, "Identifier")));
}

// ── Default-constructed cursor ────────────────────────────────────────────

TEST(SchemaCursor, DefaultConstructedCursorIsInvalid) {
    SchemaCursor c;
    EXPECT_FALSE(c.valid());
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
    EXPECT_EQ(schema->slotKind(invalid), detail::SlotKind::End);
    EXPECT_FALSE(schema->canEndSource(invalid));
}
