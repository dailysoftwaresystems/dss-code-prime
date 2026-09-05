#include "core/types/grammar_schema.hpp"
#include "core/types/schema_walker.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// Minimal inline schema exercising every slot kind: sequence (TokenLeaf
// then RuleLeaf then End), alt (AltChoice), and a nullable rule for
// repeat semantics. Two tokens — `;` and `,` — let us drive valid +
// invalid advance scenarios.
constexpr std::string_view kWalkerSchema = R"JSON({
  "dssSchemaVersion": 1,
  "language": { "name": "W", "version": "0.1.0" },
  "tokens": {
    ";": [{ "kind": "Semi" }],
    ",": [{ "kind": "Comma" }]
  },
  "shapes": {
    "root": { "sequence": ["stmt", "Semi"] },
    "stmt": { "alt": ["Comma", "Semi"] }
  }
})JSON";

// Deeper schema for nested-rule stress tests. Each level of `level<N>`
// recursively wraps the next; bottom-of-chain matches a single Semi.
constexpr std::string_view kDeepSchema = R"JSON({
  "dssSchemaVersion": 1,
  "language": { "name": "D", "version": "0.1.0" },
  "tokens": { ";": [{ "kind": "Semi" }] },
  "shapes": {
    "root":  { "sequence": ["level1"] },
    "level1":{ "sequence": ["level2"] },
    "level2":{ "sequence": ["level3"] },
    "level3":{ "sequence": ["Semi"] }
  }
})JSON";

struct H {
    std::shared_ptr<GrammarSchema const> schema;
    RuleId rootRule;
    RuleId stmtRule;
    SchemaTokenId semiKind;
    SchemaTokenId commaKind;
};

[[nodiscard]] H load() {
    auto loaded = GrammarSchema::loadFromText(kWalkerSchema);
    EXPECT_TRUE(loaded.has_value());
    auto schema = *loaded;
    return H{
        .schema    = schema,
        .rootRule  = schema->rules().find("root"),
        .stmtRule  = schema->rules().find("stmt"),
        .semiKind  = schema->schemaTokens().find("Semi"),
        .commaKind = schema->schemaTokens().find("Comma"),
    };
}

[[nodiscard]] bool contains(std::span<SchemaTokenId const> set,
                            SchemaTokenId target) {
    return std::ranges::find(set, target) != set.end();
}

} // namespace

// ── construction ────────────────────────────────────────────────────────

TEST(SchemaWalker, DefaultStateIsInvalidCursorEmptyStack) {
    auto h = load();
    SchemaWalker w{h.schema};
    EXPECT_FALSE(w.cursor().valid());
    EXPECT_EQ(w.depth(), 0u);
    EXPECT_FALSE(w.isDesynced());
}

TEST(SchemaWalker, AdvanceOnInvalidCursorIsSilentWithoutCallback) {
    auto h = load();
    SchemaWalker w{h.schema};
    // Pre-state: cursor invalid (no enterRule). Advance on invalid →
    // schema returns invalid; noteDesync_ filters !(wasValid &&
    // !nowValid) so no callback would fire even if one were wired.
    // Without a callback, the path must not crash.
    EXPECT_FALSE(w.advance(h.semiKind, SourceSpan::empty(0),
                           std::nullopt));
    EXPECT_FALSE(w.isDesynced());
}

// ── navigation ──────────────────────────────────────────────────────────

TEST(SchemaWalker, EnterRulePushesParentAndPositionsAtRuleEntry) {
    auto h = load();
    SchemaWalker w{h.schema};
    w.enterRule(h.rootRule);
    EXPECT_TRUE(w.cursor().valid());
    EXPECT_EQ(w.depth(), 1u);
    EXPECT_EQ(w.slotKind(), SlotKind::RuleLeaf);
    EXPECT_EQ(w.slotRuleRef(), h.stmtRule);
}

TEST(SchemaWalker, LeaveRuleRestoresParentCursor) {
    auto h = load();
    SchemaWalker w{h.schema};
    w.enterRule(h.rootRule);
    w.enterRule(h.stmtRule);
    EXPECT_EQ(w.depth(), 2u);

    w.advance(h.semiKind, SourceSpan::empty(0), std::nullopt);
    EXPECT_TRUE(w.isAtEndOfRule());

    w.leaveRule(SourceSpan::empty(0), h.stmtRule);
    EXPECT_EQ(w.depth(), 1u);
    // Post-stmt slot is the root's `Semi` TokenLeaf.
    EXPECT_EQ(w.slotKind(), SlotKind::TokenLeaf);
}

TEST(SchemaWalker, AdvanceConsumesTokenLeafAndReturnsValidity) {
    auto h = load();
    SchemaWalker w{h.schema};
    w.enterRule(h.rootRule);
    w.enterRule(h.stmtRule);
    const bool stillValid = w.advance(h.semiKind, SourceSpan::empty(0),
                                      std::nullopt);
    EXPECT_TRUE(stillValid);
    EXPECT_TRUE(w.isAtEndOfRule());
}

TEST(SchemaWalker, AdvanceOnValidTokenDoesNotFireDesync) {
    // Negative pin: per CLAUDE §7.1.10 "verify what isn't there."
    // A successful advance must NOT fire the desync callback.
    auto h = load();
    std::size_t fireCount = 0;
    SchemaWalker w{h.schema,
                   [&](SourceSpan, std::optional<RuleId>) { ++fireCount; }};
    w.enterRule(h.rootRule);
    w.enterRule(h.stmtRule);
    w.advance(h.semiKind, SourceSpan::empty(0), std::nullopt);
    EXPECT_EQ(fireCount, 0u)
        << "successful valid→valid advance must not trip desync emission";
}

// ── desync callback ─────────────────────────────────────────────────────

TEST(SchemaWalker, DesyncCallbackFiresOnceOnValidToInvalidTransition) {
    auto h = load();
    std::size_t fireCount = 0;
    SchemaWalker w{h.schema,
                   [&](SourceSpan, std::optional<RuleId>) { ++fireCount; }};
    w.enterRule(h.rootRule);
    w.enterRule(h.stmtRule);
    const auto bogusKind = h.schema->schemaTokens().find("Identifier");
    w.advance(bogusKind, SourceSpan::empty(0), std::nullopt);
    EXPECT_FALSE(w.cursor().valid());
    EXPECT_TRUE(w.isDesynced());
    EXPECT_EQ(fireCount, 1u);

    // Second wrong-token advance: latch holds; callback stays silent.
    w.advance(bogusKind, SourceSpan::empty(0), std::nullopt);
    EXPECT_EQ(fireCount, 1u);
}

TEST(SchemaWalker, DesyncCallbackPassesSpanAndRuleContext) {
    auto h = load();
    SourceSpan seenSpan = SourceSpan::empty(0);
    std::optional<RuleId> seenRule;
    SchemaWalker w{h.schema,
                   [&](SourceSpan span, std::optional<RuleId> rule) {
                       seenSpan = span;
                       seenRule = rule;
                   }};
    w.enterRule(h.rootRule);
    w.enterRule(h.stmtRule);
    const auto bogusKind = h.schema->schemaTokens().find("Identifier");
    const auto span = SourceSpan::of(7, 8);
    w.advance(bogusKind, span, h.stmtRule);
    EXPECT_EQ(seenSpan.start(), span.start());
    EXPECT_EQ(seenSpan.end(),   span.end());
    ASSERT_TRUE(seenRule.has_value());
    EXPECT_EQ(*seenRule, h.stmtRule);
}

// Note: throwing-callback contract violation is now a fatal-abort —
// see SchemaWalkerDeath.ThrowingDesyncCallbackAborts below.

// ── snapshot / restore ──────────────────────────────────────────────────

TEST(SchemaWalker, SnapshotRestoreRoundTripsCursorStackAndLatch) {
    auto h = load();
    std::size_t fireCount = 0;
    SchemaWalker w{h.schema,
                   [&](SourceSpan, std::optional<RuleId>) { ++fireCount; }};
    w.enterRule(h.rootRule);
    w.enterRule(h.stmtRule);
    auto baseline = w.snapshot();
    const auto baselineCursor = w.cursor();
    const auto baselineDepth  = w.depth();

    const auto bogusKind = h.schema->schemaTokens().find("Identifier");
    w.advance(bogusKind, SourceSpan::empty(0), std::nullopt);
    EXPECT_TRUE(w.isDesynced());
    EXPECT_EQ(fireCount, 1u);

    w.restore(std::move(baseline));
    EXPECT_EQ(w.cursor(),  baselineCursor);
    EXPECT_EQ(w.depth(),   baselineDepth);
    EXPECT_FALSE(w.isDesynced())
        << "snapshot/restore must roll back the desync latch";

    w.advance(bogusKind, SourceSpan::empty(0), std::nullopt);
    EXPECT_EQ(fireCount, 2u)
        << "desync emission must fire again after a rollback that reset the latch";
}

TEST(SchemaWalker, NestedSpeculationPreservesLatchAcrossInnerRollback) {
    // PA1's hot-loop pattern: take a snapshot, try a branch, on
    // failure restore and try another. With nested checkpoints the
    // latch invariant is "if the OUTER snapshot was already
    // post-desync, an inner rollback must keep the walker desynced."
    auto h = load();
    std::size_t fireCount = 0;
    SchemaWalker w{h.schema,
                   [&](SourceSpan, std::optional<RuleId>) { ++fireCount; }};

    w.enterRule(h.rootRule);
    w.enterRule(h.stmtRule);

    // First desync — outer snapshot captures the desynced state.
    const auto bogusKind = h.schema->schemaTokens().find("Identifier");
    w.advance(bogusKind, SourceSpan::empty(0), std::nullopt);
    EXPECT_TRUE(w.isDesynced());
    EXPECT_EQ(fireCount, 1u);

    auto outerSnap = w.snapshot();

    // Speculation: take an inner snapshot, do nothing meaningful,
    // restore. The walker must still be desynced.
    auto innerSnap = w.snapshot();
    w.restore(std::move(innerSnap));
    EXPECT_TRUE(w.isDesynced())
        << "inner restore from a desynced snapshot must keep the latch";

    // A subsequent valid→invalid would-be transition stays silent.
    w.advance(bogusKind, SourceSpan::empty(0), std::nullopt);
    EXPECT_EQ(fireCount, 1u);

    // Restoring the OUTER snapshot — still desynced.
    w.restore(std::move(outerSnap));
    EXPECT_TRUE(w.isDesynced());
}

TEST(SchemaWalker, SpeculationLoopPattern) {
    // PA1 will write `snapshot → try → restore-on-fail → retry` loops.
    // Simulate the pattern N times and verify the walker stays
    // well-behaved: callback fires per cycle, latch resets each time,
    // depth returns to baseline.
    auto h = load();
    std::size_t fireCount = 0;
    SchemaWalker w{h.schema,
                   [&](SourceSpan, std::optional<RuleId>) { ++fireCount; }};
    w.enterRule(h.rootRule);
    w.enterRule(h.stmtRule);
    const auto baseline = w.snapshot();
    const auto baselineDepth = w.depth();
    const auto bogusKind = h.schema->schemaTokens().find("Identifier");

    constexpr int kIterations = 100;
    for (int i = 0; i < kIterations; ++i) {
        auto snap = w.snapshot();
        w.advance(bogusKind, SourceSpan::empty(0), std::nullopt);
        w.restore(std::move(snap));
        EXPECT_FALSE(w.isDesynced());
        EXPECT_EQ(w.depth(), baselineDepth);
    }
    EXPECT_EQ(fireCount, static_cast<std::size_t>(kIterations))
        << "each speculation cycle should re-fire the callback after restore";

    (void)baseline;  // suppress unused-variable
}

TEST(SchemaWalkerDeath, SnapshotDoesNotTransferBetweenWalkersOnOneSchema) {
    // ⚠ THIS CONTRACT NARROWED IN P61, AND THE NARROWING IS THE POINT.
    // A Snapshot used to be a COPY of the walker's state, so restoring one
    // into a DIFFERENT walker over the same schema compiled and "worked" —
    // which is what this case used to assert. It is now a MARK into ITS OWN
    // walker's undo journal, so a foreign walker would rewind against a
    // journal that never recorded those frames.
    //
    // Removing the capability is a strengthening, not a loss. Nothing in
    // production ever did it (both consumers snapshot and restore the same
    // walker), and the header's own contract says the cursor stack mirrors
    // the CONSUMER's frames 1:1 — so installing another consumer's stack
    // desynchronizes the one that owns it. What used to be a silent
    // desynchronization is now a fatal abort that names the confusion.
    auto h = load();
    SchemaWalker a{h.schema};
    a.enterRule(h.rootRule);
    a.enterRule(h.stmtRule);
    auto snap = a.snapshot();

    SchemaWalker b{h.schema};
    b.enterRule(h.rootRule);
    b.enterRule(h.stmtRule);
    EXPECT_DEATH(b.restore(std::move(snap)),
                 "belongs to a different walker instance");
}

TEST(SchemaWalkerDeath, DestroyingAWalkerWithALiveSnapshotAborts) {
    // The lifetime rule a raw back-pointer needs, made observable. A
    // Snapshot retires its mark from its own destructor, so one that
    // outlived its walker would touch freed memory — the walker refuses to
    // die first instead.
    auto h = load();
    EXPECT_DEATH(
        {
            std::optional<SchemaWalker::Snapshot> held;
            {
                SchemaWalker w{h.schema};
                w.enterRule(h.rootRule);
                held.emplace(w.snapshot());
            }
        },
        "destroyed while a Snapshot is still outstanding");
}

TEST(SchemaWalker, RetiringTheLastSnapshotDropsTheUndoJournal) {
    // The property that keeps a whole-file parse LINEAR rather than
    // accumulating one undo record per frame close forever, observed at the
    // WALKER's level: after every mark has retired, a long run of enter /
    // leave cycles must still rewind correctly against a FRESH mark.
    //
    // ⚠ WHAT THIS CASE CAN AND CANNOT SEE, STATED RATHER THAN CONCEDED. The
    // journal is `frames_`' business and the walker exposes no size, so the
    // arm that would red on a journal growing forever is
    // `SpeculationTrail.RepeatedMarkAndRewindCyclesDoNotAccumulateRecords`,
    // which asserts `journalSize() == 0` after each cycle on the container
    // itself. What is checkable HERE is that the rewind stays EXACT once the
    // journal has been dropped and re-armed many times — so the assertions
    // below compare the restored CURSOR and expected set, not just the stack
    // depth. A length-only check was the previous shape and it would have
    // passed on a stack of the right height holding the wrong frames.
    auto h = load();
    SchemaWalker w{h.schema};
    w.enterRule(h.rootRule);
    const SchemaCursor atRoot = w.cursor();
    ASSERT_TRUE(atRoot.valid());

    for (int cycle = 0; cycle < 200; ++cycle) {
        auto snap = w.snapshot();
        w.enterRule(h.stmtRule);
        w.leaveRule(SourceSpan::empty(0), h.stmtRule);
        w.leaveRule(SourceSpan::empty(0), h.rootRule);   // BELOW the mark
        w.enterRule(h.stmtRule);                          // and refill
        w.restore(std::move(snap));
        ASSERT_EQ(w.depth(), 1u) << "cycle " << cycle;
        // The frame CONTENTS, not merely the count: a stack rebuilt from a
        // stale journal would come back the right height around the `stmt`
        // frame the branch pushed over the vacated slot.
        ASSERT_EQ(w.cursor(), atRoot) << "cycle " << cycle;
        ASSERT_FALSE(w.isDesynced()) << "cycle " << cycle;
    }
    EXPECT_EQ(w.depth(), 1u);
    EXPECT_EQ(w.cursor(), atRoot);
}

// Compile-time pin: Snapshot must be non-default-constructible so a
// stray Snapshot value cannot reach `restore()` carrying a null
// schemaPtr_.
static_assert(!std::is_default_constructible_v<SchemaWalker::Snapshot>,
              "SchemaWalker::Snapshot must be non-default-constructible — "
              "every instance must originate from a live walker.snapshot() call");
static_assert(!std::is_copy_constructible_v<SchemaWalker::Snapshot>,
              "SchemaWalker::Snapshot must be move-only");
static_assert(std::is_move_constructible_v<SchemaWalker::Snapshot>,
              "SchemaWalker::Snapshot must be movable for opaque transfer");

// ── introspection passthroughs ──────────────────────────────────────────

TEST(SchemaWalker, IntrospectionDelegatesToSchema) {
    auto h = load();
    SchemaWalker w{h.schema};
    // Pre-entry: invalid cursor → schema's slotKind convention is End.
    EXPECT_EQ(w.slotKind(), SlotKind::End);

    w.enterRule(h.rootRule);
    // root's expectedSet at entry is FIRST(stmt) = {Comma, Semi}.
    const auto expected = w.expectedSet();
    EXPECT_TRUE(contains(expected, h.commaKind))
        << "FIRST(stmt) must include Comma";
    EXPECT_TRUE(contains(expected, h.semiKind))
        << "FIRST(stmt) must include Semi";

    w.enterRule(h.stmtRule);
    EXPECT_EQ(w.slotKind(), SlotKind::AltChoice);
    EXPECT_FALSE(w.isSpeculativeAlt());
    EXPECT_EQ(w.lookahead(), 0u);
}

TEST(SchemaWalker, CanEndSourceFiresOnlyAtRootEnd) {
    auto h = load();
    SchemaWalker w{h.schema};
    EXPECT_FALSE(w.canEndSource());

    w.enterRule(h.rootRule);
    EXPECT_FALSE(w.canEndSource()) << "mid-root: still expecting children";

    w.enterRule(h.stmtRule);
    w.advance(h.semiKind, SourceSpan::empty(0), std::nullopt);
    w.leaveRule(SourceSpan::empty(0), h.stmtRule);
    w.advance(h.semiKind, SourceSpan::empty(0), std::nullopt);
    EXPECT_TRUE(w.canEndSource())
        << "at root's End with nullableTail, source can terminate";
}

// ── move semantics ──────────────────────────────────────────────────────

TEST(SchemaWalker, MoveConstructionTransfersState) {
    auto h = load();
    SchemaWalker a{h.schema};
    a.enterRule(h.rootRule);
    const auto cursorBefore = a.cursor();
    const auto depthBefore  = a.depth();

    SchemaWalker b{std::move(a)};
    EXPECT_EQ(b.cursor(), cursorBefore);
    EXPECT_EQ(b.depth(),  depthBefore);
}

TEST(SchemaWalker, MoveAssignmentReplacesStateAndResetsLatch) {
    // Move-assign over a desynced walker: the LHS adopts the RHS's
    // cursor + stack + latch verbatim. A defaulted move-assign that
    // left stale state on the LHS would silently mute future desyncs.
    auto h = load();
    std::size_t lhsFires = 0;
    std::size_t rhsFires = 0;

    SchemaWalker lhs{h.schema,
                     [&](SourceSpan, std::optional<RuleId>) { ++lhsFires; }};
    lhs.enterRule(h.rootRule);
    lhs.enterRule(h.stmtRule);
    const auto bogusKind = h.schema->schemaTokens().find("Identifier");
    lhs.advance(bogusKind, SourceSpan::empty(0), std::nullopt);
    EXPECT_TRUE(lhs.isDesynced());
    EXPECT_EQ(lhsFires, 1u);

    SchemaWalker rhs{h.schema,
                     [&](SourceSpan, std::optional<RuleId>) { ++rhsFires; }};
    rhs.enterRule(h.rootRule);

    lhs = std::move(rhs);
    EXPECT_FALSE(lhs.isDesynced())
        << "move-assign over desynced walker must adopt RHS's clean latch";
    EXPECT_EQ(lhs.depth(), 1u)
        << "move-assign must adopt RHS's stack depth (1 from enterRule)";

    // Subsequent desync on the move-assigned walker fires the RHS
    // callback (the one moved in), not the LHS one.
    lhs.enterRule(h.stmtRule);
    lhs.advance(bogusKind, SourceSpan::empty(0), std::nullopt);
    EXPECT_EQ(rhsFires, 1u)
        << "desync after move-assign must fire RHS's callback";
    EXPECT_EQ(lhsFires, 1u)
        << "LHS's original callback must not fire after move-assign";
}

// ── deep nesting (PA1 stress) ───────────────────────────────────────────

TEST(SchemaWalker, DeepNestingRoundTripsCleanly) {
    auto loaded = GrammarSchema::loadFromText(kDeepSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    const RuleId root   = schema->rules().find("root");
    const RuleId level1 = schema->rules().find("level1");
    const RuleId level2 = schema->rules().find("level2");
    const RuleId level3 = schema->rules().find("level3");
    const auto   semi   = schema->schemaTokens().find("Semi");

    SchemaWalker w{schema};
    w.enterRule(root);
    w.enterRule(level1);
    w.enterRule(level2);
    w.enterRule(level3);
    EXPECT_EQ(w.depth(), 4u);
    w.advance(semi, SourceSpan::empty(0), std::nullopt);
    w.leaveRule(SourceSpan::empty(0), level3);
    w.leaveRule(SourceSpan::empty(0), level2);
    w.leaveRule(SourceSpan::empty(0), level1);
    w.leaveRule(SourceSpan::empty(0), root);
    EXPECT_EQ(w.depth(), 0u);
    EXPECT_FALSE(w.isDesynced());
}

// ── death tests for fatal-abort paths ───────────────────────────────────

TEST(SchemaWalkerDeath, EnterRuleWithInvalidRuleAborts) {
    auto h = load();
    SchemaWalker w{h.schema};
    EXPECT_DEATH(w.enterRule(InvalidRule), "enterRule");
}

TEST(SchemaWalkerDeath, ThrowingDesyncCallbackAborts) {
    // Contract: DesyncCallback MUST NOT throw. Violation fatal-aborts
    // with a "contract requires no-throw" message — matches the
    // discipline applied to other walker-contract violations
    // (`enterRule(InvalidRule)`, `leaveRule` underflow, cross-walker
    // `restore`).
    auto h = load();
    SchemaWalker w{h.schema,
                   [](SourceSpan, std::optional<RuleId>) {
                       throw std::runtime_error("contract violation");
                   }};
    w.enterRule(h.rootRule);
    w.enterRule(h.stmtRule);
    const auto bogusKind = h.schema->schemaTokens().find("Identifier");
    EXPECT_DEATH(w.advance(bogusKind, SourceSpan::empty(0), std::nullopt),
                 "no-throw");
}

TEST(SchemaWalkerDeath, LeaveRuleOnEmptyStackAborts) {
    auto h = load();
    SchemaWalker w{h.schema};
    EXPECT_DEATH(w.leaveRule(SourceSpan::empty(0), std::nullopt),
                 "underflow");
}

TEST(SchemaWalkerDeath, RestoreFromDifferentWalkerAborts) {
    auto h = load();
    // Two walkers over DIFFERENT schemas — cross-walker restore is
    // the silent-corruption path the guard catches.
    auto otherLoaded = GrammarSchema::loadFromText(kDeepSchema);
    ASSERT_TRUE(otherLoaded.has_value());
    auto otherSchema = *otherLoaded;

    SchemaWalker a{otherSchema};
    a.enterRule(otherSchema->rules().find("root"));
    auto crossSnap = a.snapshot();

    SchemaWalker b{h.schema};
    EXPECT_DEATH(b.restore(std::move(crossSnap)),
                 "schema pointer mismatch");
}

// ── The wrap depth must survive a speculative rewind ─────────────────────
//
// ★ THE INVARIANT NOTHING CHECKED. `wrapDepth_` — the O(1) "is any ancestor
// frame a Pratt auto-interned wrapper" counter that SUPPRESSES the desync
// latch — is restored from a SCALAR carried in the Snapshot, while the frame
// stack it must agree with is rewound from the undo journal. Two restores from
// two places, and until `assertWrapDepthMatchesFrames_` nothing related them.
// The predecessor design kept the wrap flags in a vector parallel to the
// cursor stack and fail-loud checked that the two LENGTHS agreed on restore;
// folding the flag into the frame made that check vacuous and it was retired
// with nothing put in its place.
//
// ⚠ AND NO EXISTING CASE COULD HAVE SEEN IT. Every snapshot case above walks
// `stmt`, for which `isAutoInternedWrapperRule` answers FALSE — so the wrap
// count is zero on both sides of every rewind in this file and any restore of
// it, correct or not, agrees. This schema declares real `expr.wrapperRules`,
// so `bExpr` below is auto-interned by the loader and IS a wrapper.
constexpr std::string_view kWrapSchema = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "WrapW", "version": "0.1.0" },
  "tokens": {
    ";": [{ "kind": "Semi" }],
    ",": [{ "kind": "Comma" }]
  },
  "shapes": {
    "root":       { "sequence": ["expression", "Semi"] },
    "expression": {
      "expr": {
        "atom": "operand",
        "wrapperRules": { "binary": "bExpr", "unary": "uExpr", "postfix": "pExpr" }
      }
    },
    "operand":    { "alt": ["Identifier"] }
  }
})JSON";

TEST(SchemaWalker, WrapDepthSurvivesARewindThatDrainedBelowTheMark) {
    auto loaded = GrammarSchema::loadFromText(kWrapSchema);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto schema = *loaded;

    const RuleId rootRule    = schema->rules().find("root");
    const RuleId operandRule = schema->rules().find("operand");
    const RuleId exprRule    = schema->rules().find("expression");
    ASSERT_TRUE(rootRule.valid());
    ASSERT_TRUE(operandRule.valid());
    ASSERT_TRUE(exprRule.valid());
    const auto pack = schema->exprWrapperRules(exprRule);
    ASSERT_TRUE(pack.valid());
    // The premise, asserted rather than assumed: `bExpr` IS an auto-interned
    // wrapper and `operand` is not. If the loader ever stops interning
    // wrappers this case must red HERE, not by silently comparing two
    // ordinary rules and reporting agreement.
    ASSERT_TRUE(schema->isAutoInternedWrapperRule(pack.binary));
    ASSERT_FALSE(schema->isAutoInternedWrapperRule(operandRule));

    const SchemaTokenId comma = schema->schemaTokens().find("Comma");
    ASSERT_TRUE(comma.valid());

    // One arm of the experiment. `middle` is the frame whose wrap-ness is
    // under test, and a REAL rule is opened UNDER it so the cursor is valid
    // going in — which is what makes the advance below a genuine
    // valid→invalid transition instead of a no-op the latch never sees.
    // Snapshot, drain the stack BELOW the mark and refill it with non-wrap
    // frames, rewind, then force the transition and report whether the desync
    // latch fired. Suppression is exactly what `wrapDepth_` decides, so the
    // count IS the restored wrap depth, observed through behaviour.
    auto desyncsAfterRewind = [&](RuleId middle) {
        int fired = 0;
        SchemaWalker w{schema, [&](SourceSpan, std::optional<RuleId>) { ++fired; }};
        w.enterRule(rootRule);
        w.enterRule(middle);
        w.enterRule(operandRule);
        auto snap = w.snapshot();
        w.leaveRule(SourceSpan::empty(0), operandRule);
        w.leaveRule(SourceSpan::empty(0), middle);
        w.leaveRule(SourceSpan::empty(0), rootRule);   // BELOW the mark
        w.enterRule(rootRule);                         // ... and refill it,
        w.enterRule(operandRule);                      // with frames that are
        w.enterRule(operandRule);                      // NOT wraps
        w.restore(std::move(snap));
        EXPECT_EQ(w.depth(), 3u);
        EXPECT_FALSE(w.isDesynced()) << "the rewind must re-arm the latch";
        // Count only what happens AFTER the rewind: the branch above can trip
        // the callback on its own, and folding that in would make the two arms
        // differ for a reason that is not the subject.
        fired = 0;
        // ⚠ THE PRECONDITION IS ASSERTED, NOT ASSUMED — this case was VACUOUS
        // once and its own red-on-disable arm caught it. The first rendition
        // opened the wrapper and advanced straight out of it; a wrapper rule
        // has no body in the position graph, so the cursor was ALREADY invalid
        // and `noteDesync_` returned before ever consulting the wrap depth. It
        // reported 0 for the wrong reason and stayed GREEN under a mutant that
        // deleted the wrap-depth restore outright.
        EXPECT_TRUE(w.cursor().valid())
            << "the advance below must be a valid->invalid transition or this "
               "case measures nothing";
        const bool stillValid = w.advance(comma, SourceSpan::empty(0), middle);
        EXPECT_FALSE(stillValid)
            << "the advance must invalidate the cursor or the latch is never "
               "consulted";
        return fired;
    };

    // CONTROL first, so "0 everywhere" cannot pass for a result: with an
    // ordinary rule in the middle the same transition MUST reach the latch.
    EXPECT_EQ(desyncsAfterRewind(operandRule), 1)
        << "the control arm never desynced, so the wrap arm's 0 proves nothing";
    EXPECT_EQ(desyncsAfterRewind(pack.binary), 0)
        << "the rewind lost the wrap frame's depth: the desync latch fired "
           "under an auto-interned wrapper rule, where cursor invalidation is "
           "structural noise rather than a real grammar mismatch";
}
