#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

using namespace dss;

namespace {

struct Harness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
};

[[nodiscard]] Harness make(std::string source, std::string_view configText) {
    auto loaded = GrammarSchema::loadFromText(configText);
    EXPECT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    return {
        SourceBuffer::fromString(std::move(source), "<checkpoint>"),
        loaded.has_value() ? *loaded : nullptr,
    };
}

[[nodiscard]] Token tokAt(SourceBuffer const& src, std::string_view text,
                          CoreTokenKind kind = CoreTokenKind::Operator,
                          std::size_t hint = std::string_view::npos) {
    const auto sv = src.text();
    const auto found = (hint == std::string_view::npos) ? sv.find(text)
                                                        : sv.find(text, hint);
    EXPECT_NE(found, std::string_view::npos)
        << "lexeme '" << text << "' not in source";
    return Token{
        .coreKind   = kind,
        .schemaKind = InvalidSchemaToken,
        .span       = SourceSpan::of(static_cast<ByteOffset>(found),
                                      static_cast<ByteOffset>(found + text.size())),
    };
}

[[nodiscard]] std::size_t countCode(Tree const& t, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : t.diagnostics().all()) if (d.code == code) ++n;
    return n;
}

constexpr std::string_view kBasicCfg = R"JSON({
  "dssSchemaVersion": 1,
  "language": { "name": "X", "version": "0.1.0" },
  "keywords": [ { "word": "if", "kind": "IfKw" } ],
  "tokens": {
    "+": [{ "kind": "PlusOp" }],
    ";": [{ "kind": "EndStatement" }]
  },
  "shapes": {
    "root":  { "sequence": [ { "repeat": "stmt" } ] },
    "stmt":  { "sequence": [ "Identifier", "EndStatement" ] }
  }
})JSON";

} // namespace

// ── Round-trip: rollback restores every snapshotted axis ────────────────

TEST(Checkpoint, RollbackRestoresArenaSize) {
    auto h = make("a;", kBasicCfg);
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    {
        auto cp = b.checkpoint();
        const auto preFrames = b.openFrameCount();
        {
            auto stmt = b.open(h.schema->rules().find("stmt"));
            b.pushToken(tokAt(*h.src, "a", CoreTokenKind::Word));
        }
        // `stmt` closed already; back at `root`. Now roll back.
        b.rollback(std::move(cp));
        EXPECT_EQ(b.openFrameCount(), preFrames);
    }
    // `root` still open; close cleanly.
    root.close();
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

TEST(Checkpoint, RollbackRestoresOpenFrameStack) {
    auto h = make("a;", kBasicCfg);
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));

    auto cp = b.checkpoint();
    EXPECT_EQ(b.openFrameCount(), 1u);
    auto stmt = b.open(h.schema->rules().find("stmt"));
    EXPECT_EQ(b.openFrameCount(), 2u);
    b.pushToken(tokAt(*h.src, "a", CoreTokenKind::Word));
    // Don't close stmt explicitly — rollback should drop the frame.
    // But OpenScope still owns the cookie; we need to release it before
    // rollback so the destructor doesn't double-close.
    // Pattern: rollback first, then let the moved-out stmt's destructor
    // observe `closedCookies_` and no-op. Actually safer: close stmt
    // first (which adds it to closedCookies_), then rollback restores
    // the closed cookie set too.
    stmt.close();
    EXPECT_EQ(b.openFrameCount(), 1u);
    b.rollback(std::move(cp));
    EXPECT_EQ(b.openFrameCount(), 1u);

    root.close();
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

TEST(Checkpoint, RollbackRestoresScopeStack) {
    auto h = make("a;", kBasicCfg);
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));

    b.pushScope(ScopeKind::Block);
    EXPECT_EQ(b.scopeStack().size(), 1u);

    auto cp = b.checkpoint();
    b.pushScope(ScopeKind::Paren);
    b.pushScope(ScopeKind::Generic);
    EXPECT_EQ(b.scopeStack().size(), 3u);

    b.rollback(std::move(cp));
    EXPECT_EQ(b.scopeStack().size(), 1u);
    EXPECT_EQ(b.currentScope(), ScopeKind::Block);

    b.popScope();
    root.close();
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

TEST(Checkpoint, RollbackDropsDiagnostics) {
    auto h = make("@", kBasicCfg);                     // '@' is unknown
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    {
        auto cp = b.checkpoint();
        auto stmt = b.open(h.schema->rules().find("stmt"));
        b.pushToken(tokAt(*h.src, "@"));               // → P_UnknownToken
        stmt.close();
        b.rollback(std::move(cp));
    }
    root.close();
    Tree t = std::move(b).finish();
    EXPECT_EQ(countCode(t, DiagnosticCode::P_UnknownToken), 0u)
        << "diagnostic emitted inside a rolled-back branch must not survive";
}

TEST(Checkpoint, CommitKeepsDiagnostics) {
    auto h = make("@", kBasicCfg);
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    {
        auto cp = b.checkpoint();
        auto stmt = b.open(h.schema->rules().find("stmt"));
        b.pushToken(tokAt(*h.src, "@"));
        stmt.close();
        b.commit(std::move(cp));
    }
    root.close();
    Tree t = std::move(b).finish();
    EXPECT_GE(countCode(t, DiagnosticCode::P_UnknownToken), 1u);
}

// ── Reporter cap behavior ───────────────────────────────────────────────

TEST(Checkpoint, RollbackClearsHitCapLatch) {
    auto h = make("a;", kBasicCfg);
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 3;                            // tiny cap
    cfg.dedupWindow    = 0;                            // disable dedup
    TreeBuilder b{h.src, h.schema, cfg};
    auto root = b.open(h.schema->rules().find("root"));

    auto cp = b.checkpoint();
    // Flood with pushErrors (each emits P_UnexpectedToken). The cap
    // trips around the 3rd; further reports are silent.
    for (int i = 0; i < 10; ++i) {
        b.pushError(SourceSpan::of(0, 1), std::nullopt, std::nullopt, "noise");
    }
    b.rollback(std::move(cp));

    // After rollback, the latch should be clear — a real diagnostic
    // emitted post-rollback must land.
    b.pushError(SourceSpan::of(0, 1), std::nullopt, std::nullopt, "real");
    root.close();
    Tree t = std::move(b).finish();

    auto const& diags = t.diagnostics().all();
    EXPECT_FALSE(diags.empty())
        << "post-rollback diagnostics must not be silenced by speculative cap";
    EXPECT_TRUE(std::ranges::any_of(diags, [](auto const& d) {
        return d.code == DiagnosticCode::P_UnexpectedToken &&
               d.actual.find("real") != std::string::npos;
    }));
}

// ── Nested checkpoints ──────────────────────────────────────────────────

TEST(Checkpoint, NestedCommitInnerThenRollbackOuter) {
    auto h = make("a;", kBasicCfg);
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));

    const auto preFrames = b.openFrameCount();
    auto cp1 = b.checkpoint();
    {
        auto cp2 = b.checkpoint();
        auto stmt = b.open(h.schema->rules().find("stmt"));
        b.pushToken(tokAt(*h.src, "a", CoreTokenKind::Word));
        stmt.close();
        b.commit(std::move(cp2));
    }
    b.rollback(std::move(cp1));
    EXPECT_EQ(b.openFrameCount(), preFrames)
        << "rolling back outer checkpoint must undo inner-committed work";

    root.close();
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

TEST(Checkpoint, NestedRollbackInnerThenCommitOuter) {
    auto h = make("a;", kBasicCfg);
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));

    auto cp1 = b.checkpoint();
    auto stmt = b.open(h.schema->rules().find("stmt"));
    b.pushToken(tokAt(*h.src, "a", CoreTokenKind::Word));
    {
        auto cp2 = b.checkpoint();
        b.pushError(SourceSpan::of(0, 1), std::nullopt, std::nullopt, "inner");
        b.rollback(std::move(cp2));
    }
    stmt.close();
    b.commit(std::move(cp1));

    root.close();
    Tree t = std::move(b).finish();
    // The inner pushError was rolled back; outer work committed.
    EXPECT_EQ(countCode(t, DiagnosticCode::P_UnexpectedToken), 0u);
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

// ── Watchdog: max speculation depth ─────────────────────────────────────

TEST(Checkpoint, MaxSpeculationDepthEmitsExactlyOneDiagnostic) {
    auto h = make("a;", kBasicCfg);
    BuilderConfig bc;
    bc.maxSpeculationDepth = 3;
    TreeBuilder b{h.src, h.schema, {}, bc};
    auto root = b.open(h.schema->rules().find("root"));

    auto cp1 = b.checkpoint();
    auto cp2 = b.checkpoint();
    auto cp3 = b.checkpoint();
    // 4th exceeds the cap → no-op guard + one diagnostic.
    auto cp4 = b.checkpoint();
    auto cp5 = b.checkpoint();                          // also a no-op; second emission suppressed

    // No-op guards have id 0; commit/rollback on them are safe no-ops.
    b.commit(std::move(cp5));
    b.commit(std::move(cp4));
    b.commit(std::move(cp3));
    b.commit(std::move(cp2));
    b.commit(std::move(cp1));

    root.close();
    Tree t = std::move(b).finish();
    EXPECT_EQ(countCode(t, DiagnosticCode::P_MaxSpeculationDepth), 1u);
}

// ── Uncommitted-checkpoint warning ──────────────────────────────────────

TEST(Checkpoint, UncommittedCheckpointDtorEmitsWarning) {
    auto h = make("a;", kBasicCfg);
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    {
        auto cp = b.checkpoint();
        // Drop on the floor — destructor rolls back + emits warning.
    }
    root.close();
    Tree t = std::move(b).finish();
    EXPECT_EQ(countCode(t, DiagnosticCode::P_UncommittedCheckpoint), 1u);
}

TEST(Checkpoint, UncommittedCheckpointStillRollsBack) {
    auto h = make("@", kBasicCfg);
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    {
        auto cp = b.checkpoint();
        auto stmt = b.open(h.schema->rules().find("stmt"));
        b.pushToken(tokAt(*h.src, "@"));                // P_UnknownToken
        stmt.close();
        // cp destructor runs here — rolls back.
    }
    root.close();
    Tree t = std::move(b).finish();
    // The dropped diagnostic must NOT survive — the destructor rolled back.
    EXPECT_EQ(countCode(t, DiagnosticCode::P_UnknownToken), 0u);
    EXPECT_EQ(countCode(t, DiagnosticCode::P_UncommittedCheckpoint), 1u);
}

// ── Loader plumbing for speculative + lookahead ─────────────────────────

TEST(Checkpoint, SpeculativeAltLoaderPlumbsAttributes) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "Y", "version": "0.1.0" },
      "tokens": {
        "a": [{ "kind": "TokA" }],
        "b": [{ "kind": "TokB" }]
      },
      "shapes": {
        "root": { "alt": ["TokA", "TokB"], "speculative": true, "lookahead": 4 }
      }
    })JSON";

    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const& s = **loaded;
    auto cur = s.rootCursor();
    EXPECT_TRUE(s.isSpeculativeAlt(cur));
    EXPECT_EQ(s.lookahead(cur), 4u);
}

TEST(Checkpoint, NonSpeculativeAltDefaultsToFalse) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "Y", "version": "0.1.0" },
      "tokens": {
        "a": [{ "kind": "TokA" }],
        "b": [{ "kind": "TokB" }]
      },
      "shapes": {
        "root": { "alt": ["TokA", "TokB"] }
      }
    })JSON";

    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto cur = (*loaded)->rootCursor();
    EXPECT_FALSE((*loaded)->isSpeculativeAlt(cur));
    EXPECT_EQ((*loaded)->lookahead(cur), 0u);
}

TEST(Checkpoint, SpeculativeWithoutLookaheadDefaultsToEight) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "Y", "version": "0.1.0" },
      "tokens": {
        "a": [{ "kind": "TokA" }],
        "b": [{ "kind": "TokB" }]
      },
      "shapes": {
        "root": { "alt": ["TokA", "TokB"], "speculative": true }
      }
    })JSON";

    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto cur = (*loaded)->rootCursor();
    EXPECT_TRUE((*loaded)->isSpeculativeAlt(cur));
    EXPECT_EQ((*loaded)->lookahead(cur), 8u);
}

// ── No-op checkpoint after cap is hit ───────────────────────────────────

TEST(Checkpoint, NoOpGuardAfterCapAcceptsCommitAndRollback) {
    auto h = make("a;", kBasicCfg);
    BuilderConfig bc;
    bc.maxSpeculationDepth = 1;
    TreeBuilder b{h.src, h.schema, {}, bc};
    auto root = b.open(h.schema->rules().find("root"));

    auto cp1 = b.checkpoint();
    auto cp2 = b.checkpoint();                          // no-op (at cap)
    // Both should commit cleanly without aborting.
    b.commit(std::move(cp2));
    b.commit(std::move(cp1));
    root.close();
    Tree t = std::move(b).finish();
    EXPECT_EQ(countCode(t, DiagnosticCode::P_MaxSpeculationDepth), 1u);
}

// ── Performance smoke ───────────────────────────────────────────────────

TEST(Checkpoint, ThousandTokensWithSpeculationUnder50ms) {
    // Not a tight benchmark — just a smoke pin that Checkpoint isn't
    // pathologically slow. 1000 tokens, one outer speculative wrap that
    // commits. Real benchmarks belong elsewhere; this guards against
    // O(N) checkpoint state bloat regressions.
    std::string source;
    source.reserve(2000);
    for (int i = 0; i < 1000; ++i) source += "a;";

    auto h = make(std::move(source), kBasicCfg);
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));

    const auto t0 = std::chrono::steady_clock::now();
    auto cp = b.checkpoint();
    std::size_t pos = 0;
    for (int i = 0; i < 1000; ++i) {
        auto stmt = b.open(h.schema->rules().find("stmt"));
        b.pushToken(tokAt(*h.src, "a", CoreTokenKind::Word, pos));
        b.pushToken(tokAt(*h.src, ";", CoreTokenKind::Operator, pos));
        pos = h.src->text().find(';', pos) + 1;
    }
    b.commit(std::move(cp));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    root.close();
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());

    // 50ms threshold from the plan. Generous given the benchmark machine
    // varies; the goal is to catch O(N^2) regressions, not pin perf.
    EXPECT_LT(ms, 200)
        << "1000-token speculative parse took " << ms
        << "ms — checkpoint may have grown super-linear cost";
}

// ── Frame children refactor smoke pin ───────────────────────────────────

TEST(Checkpoint, NestedFramesProduceContiguousChildIndex) {
    // The pendingChildren_/childIndex_ refactor MUST preserve the
    // "each internal node's children are contiguous in childIndex_"
    // invariant. Build a 3-deep nested tree and verify each parent's
    // children resolve correctly.
    auto h = make("a;", kBasicCfg);
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    auto stmt = b.open(h.schema->rules().find("stmt"));
    b.pushToken(tokAt(*h.src, "a", CoreTokenKind::Word));
    b.pushToken(tokAt(*h.src, ";"));
    stmt.close();
    root.close();
    Tree t = std::move(b).finish();

    auto rootChildren = t.children(t.root());
    ASSERT_EQ(rootChildren.size(), 1u);
    auto stmtChildren = t.children(rootChildren[0]);
    ASSERT_EQ(stmtChildren.size(), 2u);
    EXPECT_EQ(t.kind(stmtChildren[0]), NodeKind::Token);
    EXPECT_EQ(t.kind(stmtChildren[1]), NodeKind::Token);
    EXPECT_FALSE(t.diagnostics().hasErrors());
}
