#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"
#include "core/types/tree_node.hpp"
#include "toy_harness.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::tests::ToyHarness;

namespace {

// Shared toy config used across nearly every test. Defined as a string so
// each test gets a fresh schema instance — frozen interners + immutable
// shape table don't change across runs, but constructing fresh keeps tests
// independent.
constexpr std::string_view kToyConfig = R"JSON({
  "dssSchemaVersion": 1,
  "language": { "name": "TestToy", "version": "0.1.0", "fileExtensions": [".t"] },
  "tokens": {
    " ":  [{ "kind": "Whitespace",   "flags": ["EmptySpace"] }],
    "+":  [
      { "kind": "SumOperator",          "priority": 10 },
      { "kind": "StringAppendOperator", "priority": 20 }
    ],
    "{":  [{ "kind": "BlockOpen",   "opensScope": "Block" }],
    "}":  [{ "kind": "BlockClose",  "closesScope": true }],
    ";":  [{ "kind": "EndCommand" }],
    "=":  [{ "kind": "AssignmentOperator" }],
    "(":  [{ "kind": "ParenOpen",   "opensScope": "Paren" }],
    ")":  [{ "kind": "ParenClose",  "closesScope": true }],
    "<":  [
      { "kind": "LtOperator",              "priority": 10 },
      { "kind": "GenericDefinitionOpener", "priority": 5, "opensScope": "Generic" }
    ]
  },
  "keywords": [
    { "word": "var", "kind": "VarKeyword" },
    { "word": "if",  "kind": "IfKeyword" }
  ],
  "scopes": {
    "validity": [
      { "scope": "Generic", "forbid": ["LtOperator"] }
    ]
  },
  "shapes": {
    "root":       { "sequence": [{ "repeat": "statement" }] },
    "statement":  { "alt": ["varDecl", "exprStmt"] },
    "varDecl":    { "sequence": ["VarKeyword", "Identifier", "AssignmentOperator", "expression", "EndCommand"] },
    "exprStmt":   { "sequence": ["expression", "EndCommand"] },
    "expression": { "sequence": ["Identifier"] }
  }
})JSON";

// Thin shim so existing tests keep using `Harness::make(text)` — delegates
// to the shared `ToyHarness` with this file's `kToyConfig`.
struct Harness : public ToyHarness {
    static Harness make(std::string sourceText) {
        Harness h;
        static_cast<ToyHarness&>(h) = ToyHarness::make(std::move(sourceText), kToyConfig);
        return h;
    }
};

// Count how many diagnostics in `all` carry the given code.
std::size_t countCode(std::span<ParseDiagnostic const> all, DiagnosticCode code) {
    return static_cast<std::size_t>(std::ranges::count_if(
        all, [code](ParseDiagnostic const& d) { return d.code == code; }));
}

} // namespace

// ── (a) Happy path ───────────────────────────────────────────────────────

TEST(TreeBuilder, HappyPathBuildsCleanTree) {
    auto h = Harness::make("var x;");

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        {
            auto stmt = b.open(h.schema->rules().find("statement"));
            {
                auto vd = b.open(h.schema->rules().find("varDecl"));
                b.pushToken(h.tok("var", CoreTokenKind::Word));
                b.pushToken(h.tok("x",   CoreTokenKind::Word));
                b.pushToken(h.tok(";"));
            }
        }
    }
    Tree t = std::move(b).finish();

    EXPECT_FALSE(hasError(t.flags(t.root())));
    EXPECT_EQ(t.kind(t.root()), NodeKind::Internal);
    EXPECT_EQ(t.rule(t.root()), h.schema->rules().find("root"));

    auto rootKids = t.children(t.root());
    ASSERT_EQ(rootKids.size(), 1u);
    EXPECT_EQ(t.rule(rootKids[0]), h.schema->rules().find("statement"));

    auto stmtKids = t.children(rootKids[0]);
    ASSERT_EQ(stmtKids.size(), 1u);
    EXPECT_EQ(t.rule(stmtKids[0]), h.schema->rules().find("varDecl"));

    auto vdKids = t.children(stmtKids[0]);
    ASSERT_EQ(vdKids.size(), 3u);   // var, x, ;
    EXPECT_EQ(t.kind(vdKids[0]), NodeKind::Token);
    EXPECT_EQ(t.text(vdKids[0]), "var");
    EXPECT_EQ(t.text(vdKids[1]), "x");
    EXPECT_EQ(t.text(vdKids[2]), ";");

    EXPECT_EQ(t.diagnostics().errorCount(),   0u);
    EXPECT_EQ(t.diagnostics().warningCount(), 0u);
}

// ── (b) Unexpected token → Error + diagnostic + HasError propagation ────

TEST(TreeBuilder, UnknownTokenInsertsErrorAndPropagatesHasError) {
    // Source contains a `@` that has no schema meaning anywhere.
    auto h = Harness::make("var x @");

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        {
            auto stmt = b.open(h.schema->rules().find("statement"));
            {
                auto vd = b.open(h.schema->rules().find("varDecl"));
                b.pushToken(h.tok("var", CoreTokenKind::Word));
                b.pushToken(h.tok("x",   CoreTokenKind::Word));
                b.pushToken(h.tok("@",   CoreTokenKind::Operator));   // unknown!
            }
        }
    }
    Tree t = std::move(b).finish();

    // Root must be flagged HasError because a descendant Error exists.
    EXPECT_TRUE(hasError(t.flags(t.root())));

    auto const& diags = t.diagnostics().all();
    // Exactly one unknown lexeme '@' → exactly one diagnostic.
    EXPECT_EQ(countCode(diags, DiagnosticCode::P_UnknownToken), 1u);
    EXPECT_TRUE(t.diagnostics().hasErrors());
}

TEST(TreeBuilder, ScopeStackCapturedOnDiagnostic) {
    auto h = Harness::make("@");

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushScope(ScopeKind::Block);
        b.pushScope(ScopeKind::Paren);
        b.pushToken(h.tok("@"));
        b.popScope();
        b.popScope();
    }
    Tree t = std::move(b).finish();

    auto diags = t.diagnostics().all();
    auto it = std::ranges::find_if(diags, [](ParseDiagnostic const& d) {
        return d.code == DiagnosticCode::P_UnknownToken;
    });
    ASSERT_NE(it, diags.end());
    ASSERT_EQ(it->scopeStack.size(), 2u);
    EXPECT_EQ(it->scopeStack[0], ScopeKind::Block);
    EXPECT_EQ(it->scopeStack[1], ScopeKind::Paren);
}

// ── (c) Premature EOF synthesizes Missing + per-frame diagnostic ───────

TEST(TreeBuilder, FinishSynthesizesMissingForUnclosedFrames) {
    auto h = Harness::make("var x");

    // Hold the OpenScope guards in vectors so they outlive the inner
    // scope but their close() can be deferred to builder.finish() — we
    // deliberately leak them by moving into discarded variables so the
    // builder reaches finish() with frames still open.
    TreeBuilder b{h.src, h.schema};

    // Use a heap-held vector of guards. Drop the vector AFTER finish() so
    // RAII close happens on a moved-from (no-op) state — every guard
    // becomes inert because closeFrame_ is processed by finish().
    auto guards = std::make_unique<std::vector<TreeBuilder::OpenScope>>();
    guards->push_back(b.open(h.schema->rules().find("root")));
    guards->push_back(b.open(h.schema->rules().find("statement")));
    guards->push_back(b.open(h.schema->rules().find("varDecl")));

    b.pushToken(h.tok("var", CoreTokenKind::Word));
    b.pushToken(h.tok("x",   CoreTokenKind::Word));

    // finish() runs with 3 open frames.
    Tree t = std::move(b).finish();
    // After finish(), the guards' close() will be invoked when `guards`
    // is destroyed. closeFrame_ on a builder that has finished is a no-op
    // because open_ is already empty. So this is safe.
    guards.reset();

    auto diags = t.diagnostics().all();
    // One P_PrematureEndOfInput per open frame at the time of finish().
    EXPECT_EQ(countCode(diags, DiagnosticCode::P_PrematureEndOfInput), 3u);
    EXPECT_TRUE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasError(t.flags(t.root())));
}

// ── (d) EmptySpace flag passthrough ──────────────────────────────────────

TEST(TreeBuilder, EmptySpaceTokensCarryFlag) {
    auto h = Harness::make("var x ;");

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto vd   = b.open(h.schema->rules().find("varDecl"));
        b.pushToken(h.tok("var", CoreTokenKind::Word));
        b.pushToken(h.tok(" ",   3, CoreTokenKind::Whitespace));   // space after var
        b.pushToken(h.tok("x",   CoreTokenKind::Word));
        b.pushToken(h.tok(" ",   5, CoreTokenKind::Whitespace));   // space after x
        b.pushToken(h.tok(";"));
    }
    Tree t = std::move(b).finish();

    auto rootKids = t.children(t.root());
    auto stmtKids = t.children(rootKids[0]);
    auto vdKids   = t.children(stmtKids[0]);
    ASSERT_EQ(vdKids.size(), 5u);   // var, " ", x, " ", ;

    EXPECT_FALSE(isEmptySpace(t.flags(vdKids[0])));    // var
    EXPECT_TRUE (isEmptySpace(t.flags(vdKids[1])));    // " "
    EXPECT_FALSE(isEmptySpace(t.flags(vdKids[2])));    // x
    EXPECT_TRUE (isEmptySpace(t.flags(vdKids[3])));    // " "
    EXPECT_FALSE(isEmptySpace(t.flags(vdKids[4])));    // ;
}

// ── (e) Ambiguous-token tiebreak ─────────────────────────────────────────

TEST(TreeBuilder, AmbiguousMeaningsTieBreakOnFirstDeclared) {
    // Build a temporary schema where `?` has TWO meanings with the SAME
    // priority. First-declared wins; we expect a P_AmbiguousToken warning.
    constexpr std::string_view amb = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "Amb", "version": "0.1.0" },
      "tokens": {
        "?": [
          { "kind": "MeaningA", "priority": 10 },
          { "kind": "MeaningB", "priority": 10 }
        ]
      },
      "shapes": { "root": { "sequence": ["Identifier"] } }
    })JSON";
    auto schema = *GrammarSchema::loadFromText(amb);
    auto src    = SourceBuffer::fromString("?", "<amb>");

    TreeBuilder b{src, schema};
    {
        auto root = b.open(schema->rules().find("root"));
        Token tok{
            .coreKind = CoreTokenKind::Operator,
            .schemaKind = InvalidSchemaToken,
            .span = SourceSpan::of(0, 1),
        };
        b.pushToken(tok);
    }
    Tree t = std::move(b).finish();

    auto diags = t.diagnostics().all();
    // Exactly one ambiguous token resolution → exactly one diagnostic.
    EXPECT_EQ(countCode(diags, DiagnosticCode::P_AmbiguousToken), 1u);

    // The first-declared meaning (MeaningA) wins.
    auto kids = t.children(t.root());
    ASSERT_EQ(kids.size(), 1u);
    EXPECT_EQ(t.kind(kids[0]), NodeKind::Token);
    EXPECT_EQ(schema->schemaTokens().name(t.tokenKind(kids[0])), "MeaningA");
}

// ── (f) Forward progress on pathological input ───────────────────────────

TEST(TreeBuilder, PathologicalInputDoesNotStall) {
    auto h = Harness::make("@@@@@@@@@@");  // 10 unknown lexemes

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        for (std::size_t i = 0; i < 10; ++i) {
            Token tok{
                .coreKind = CoreTokenKind::Operator,
                .schemaKind = InvalidSchemaToken,
                .span = SourceSpan::of(static_cast<ByteOffset>(i),
                                       static_cast<ByteOffset>(i + 1)),
            };
            b.pushToken(tok);
        }
    }
    Tree t = std::move(b).finish();

    // 10 input tokens → 10 Error leaves. Coalescing inside the reporter
    // may collapse the diagnostics, but every token must have produced a
    // leaf (so the tree spans the whole input).
    auto rootKids = t.children(t.root());
    EXPECT_EQ(rootKids.size(), 10u);
    for (NodeId child : rootKids) {
        EXPECT_EQ(t.kind(child), NodeKind::Error);
        EXPECT_TRUE(hasError(t.flags(child)));
    }
    EXPECT_TRUE(hasError(t.flags(t.root())));
}

// ── (g) OpenScope RAII semantics ─────────────────────────────────────────

TEST(TreeBuilder, OpenScopeAutoClosesOnDestruction) {
    auto h = Harness::make("var x;");

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        {
            auto stmt = b.open(h.schema->rules().find("statement"));
            auto vd   = b.open(h.schema->rules().find("varDecl"));
            b.pushToken(h.tok("var", CoreTokenKind::Word));
            b.pushToken(h.tok("x",   CoreTokenKind::Word));
            b.pushToken(h.tok(";"));
        }
    }
    Tree t = std::move(b).finish();
    EXPECT_EQ(t.diagnostics().errorCount(), 0u);
}

TEST(TreeBuilder, OpenScopeCloseIsIdempotent) {
    auto h = Harness::make("");

    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    root.close();           // explicit close
    root.close();           // idempotent: must NOT emit P_BuilderInvariant
    Tree t = std::move(b).finish();
    auto diags = t.diagnostics().all();
    EXPECT_EQ(countCode(diags, DiagnosticCode::P_BuilderInvariant), 0u);
}

// ── (h) Builder-invariant violations emit P_BuilderInvariant ────────────

TEST(TreeBuilder, PushTokenWithoutOpenFrameEmitsInvariant) {
    auto h = Harness::make("var");
    TreeBuilder b{h.src, h.schema};
    b.pushToken(h.tok("var", CoreTokenKind::Word));   // no open() called!
    Tree t = std::move(b).finish();
    EXPECT_EQ(countCode(t.diagnostics().all(),
                       DiagnosticCode::P_BuilderInvariant), 1u);
}

TEST(TreeBuilder, PopScopeUnderflowEmitsInvariant) {
    auto h = Harness::make("");
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    b.popScope();   // scope stack is empty!
    root.close();
    Tree t = std::move(b).finish();
    EXPECT_EQ(countCode(t.diagnostics().all(),
                       DiagnosticCode::P_BuilderInvariant), 1u);
}

// ── Cross-cutting: HasError propagates to root ──────────────────────────

TEST(TreeBuilder, HasErrorPropagatesUpEntireParentChain) {
    auto h = Harness::make("var x @");
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto vd   = b.open(h.schema->rules().find("varDecl"));
        b.pushToken(h.tok("var", CoreTokenKind::Word));
        b.pushToken(h.tok("x",   CoreTokenKind::Word));
        b.pushToken(h.tok("@",   CoreTokenKind::Operator));
    }
    Tree t = std::move(b).finish();

    EXPECT_TRUE(hasError(t.flags(t.root())));
    auto rootKids = t.children(t.root());
    ASSERT_FALSE(rootKids.empty());
    EXPECT_TRUE(hasError(t.flags(rootKids[0])));    // statement
    auto stmtKids = t.children(rootKids[0]);
    ASSERT_FALSE(stmtKids.empty());
    EXPECT_TRUE(hasError(t.flags(stmtKids[0])));    // varDecl
}

// ── Scope mutation by tokens ────────────────────────────────────────────

TEST(TreeBuilder, OpensScopeTokenMutatesScopeStack) {
    auto h = Harness::make("{}");
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    EXPECT_EQ(b.openFrameCount(), 1u);
    EXPECT_TRUE(b.scopeStack().empty());

    b.pushToken(h.tok("{"));
    ASSERT_EQ(b.scopeStack().size(), 1u);
    EXPECT_EQ(b.currentScope(), ScopeKind::Block);

    b.pushToken(h.tok("}"));
    EXPECT_TRUE(b.scopeStack().empty());

    root.close();
    Tree t = std::move(b).finish();
    EXPECT_EQ(t.diagnostics().errorCount(), 0u);
}

// ── pushError API ───────────────────────────────────────────────────────

TEST(TreeBuilder, PushErrorEmitsUnexpectedTokenWithExpectedFields) {
    auto h = Harness::make("var x");
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        const auto endCmdId   = h.schema->schemaTokens().find("EndCommand");
        const auto exprStmtId = h.schema->rules().find("exprStmt");
        b.pushError(SourceSpan::of(4, 5),
                    exprStmtId, endCmdId,
                    "actual='x'");
    }
    Tree t = std::move(b).finish();

    auto const& diags = t.diagnostics().all();
    auto it = std::ranges::find_if(diags, [](ParseDiagnostic const& d) {
        return d.code == DiagnosticCode::P_UnexpectedToken;
    });
    ASSERT_NE(it, diags.end());
    EXPECT_EQ(it->actual, "actual='x'");
    // Both an expected rule and an expected token were provided.
    ASSERT_EQ(it->expected.size(), 2u);
    EXPECT_EQ(it->expected[0], "exprStmt");
    EXPECT_EQ(it->expected[1], "EndCommand");

    // The Error leaf landed and HasError walked to the root.
    EXPECT_TRUE(hasError(t.flags(t.root())));
}

TEST(TreeBuilder, PushErrorWithoutOpenFrameEmitsInvariant) {
    auto h = Harness::make("x");
    TreeBuilder b{h.src, h.schema};
    b.pushError(SourceSpan::of(0, 1), std::nullopt, std::nullopt, "stray");
    Tree t = std::move(b).finish();
    EXPECT_EQ(countCode(t.diagnostics().all(),
                       DiagnosticCode::P_BuilderInvariant), 1u);
    // No spurious P_UnexpectedToken — pushError refused to fabricate one
    // without a parent to anchor the Error node.
    EXPECT_EQ(countCode(t.diagnostics().all(),
                       DiagnosticCode::P_UnexpectedToken), 0u);
}

// ── LIFO violation cascade close ───────────────────────────────────────

TEST(TreeBuilder, OutOfOrderCloseCascadesAndDoesNotDoubleDiagnose) {
    auto h = Harness::make("var x;");
    TreeBuilder b{h.src, h.schema};

    auto root = b.open(h.schema->rules().find("root"));
    auto stmt = b.open(h.schema->rules().find("statement"));
    auto vd   = b.open(h.schema->rules().find("varDecl"));

    // Close out of LIFO order: close stmt while vd is still on the stack.
    // This must:
    //   (a) cascade-close vd then stmt, emitting exactly ONE P_BuilderInvariant.
    //   (b) NOT emit a second invariant when `vd`'s destructor later runs
    //       (its cookie is now in closedCookies_).
    stmt.close();

    // vd and root will close on RAII at scope exit. vd's close() must be a
    // clean no-op (cascade already finalized its frame).
    Tree t = std::move(b).finish();

    auto const& diags = t.diagnostics().all();
    EXPECT_EQ(countCode(diags, DiagnosticCode::P_BuilderInvariant), 1u);
}

// ── OpenScope move semantics ───────────────────────────────────────────

TEST(TreeBuilder, OpenScopeMoveConstructorLeavesSourceInert) {
    auto h = Harness::make("");
    TreeBuilder b{h.src, h.schema};
    auto a = b.open(h.schema->rules().find("root"));
    EXPECT_TRUE(a.isOpen());
    auto moved = std::move(a);
    EXPECT_FALSE(a.isOpen());     // NOLINT(bugprone-use-after-move) — intentional
    EXPECT_TRUE(moved.isOpen());

    // The moved-from `a`'s close() is a no-op; `moved`'s close finalizes.
    a.close();
    moved.close();
    Tree t = std::move(b).finish();
    EXPECT_EQ(countCode(t.diagnostics().all(),
                       DiagnosticCode::P_BuilderInvariant), 0u);
}

TEST(TreeBuilder, OpenScopeMoveAssignClosesPriorFrame) {
    auto h = Harness::make("");
    TreeBuilder b{h.src, h.schema};
    auto a = b.open(h.schema->rules().find("root"));
    auto bb = b.open(h.schema->rules().find("statement"));
    // Move-assign over `a` — this should close `a`'s frame... but
    // closing the OUTER frame while INNER is still open is a LIFO
    // violation. We accept the cascade behaviour (one P_BuilderInvariant);
    // the goal here is to confirm move-assign doesn't crash and the
    // builder reaches finish() in a consistent state.
    a = std::move(bb);
    Tree t = std::move(b).finish();
    (void)t;     // smoke test — no crash, no leaks
}

// ── Double finish() ─────────────────────────────────────────────────────

TEST(TreeBuilderDeathTest, DoubleFinishAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto h = Harness::make("");
    TreeBuilder b{h.src, h.schema};
    Tree t = std::move(b).finish();
    EXPECT_DEATH({ (void)std::move(b).finish(); },
                 "finish.*twice");
}

// ── Multi-meaning token with scope side-effect ─────────────────────────

TEST(TreeBuilder, PriorityWinnerWithOpensScopeMutatesStack) {
    // In the toy config, '<' has two meanings:
    //   - GenericDefinitionOpener  priority=5  opensScope=Generic
    //   - LtOperator               priority=10
    // Lowest priority wins → GenericDefinitionOpener → pushes Generic.
    auto h = Harness::make("<");
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    EXPECT_TRUE(b.scopeStack().empty());

    b.pushToken(h.tok("<"));
    ASSERT_EQ(b.scopeStack().size(), 1u);
    EXPECT_EQ(b.currentScope(), ScopeKind::Generic);

    root.close();
    Tree t = std::move(b).finish();
    // We left Generic open — finish() flags the leftover.
    EXPECT_EQ(countCode(t.diagnostics().all(),
                       DiagnosticCode::P_BuilderInvariant), 1u);
}

// ── currentRule() introspection ─────────────────────────────────────────

TEST(TreeBuilder, CurrentRuleReturnsInvalidWhenNoFrameOpen) {
    auto h = Harness::make("");
    TreeBuilder b{h.src, h.schema};
    EXPECT_EQ(b.currentRule(), InvalidRule);
    auto root = b.open(h.schema->rules().find("root"));
    EXPECT_EQ(b.currentRule(), h.schema->rules().find("root"));
    root.close();
    EXPECT_EQ(b.currentRule(), InvalidRule);
    Tree t = std::move(b).finish();
    (void)t;
}

// ── Empty-tree finish() ─────────────────────────────────────────────────

TEST(TreeBuilder, FinishWithoutAnyOpenProducesEmptyTree) {
    auto h = Harness::make("");
    TreeBuilder b{h.src, h.schema};
    Tree t = std::move(b).finish();
    EXPECT_EQ(t.root(), InvalidNode);
    EXPECT_EQ(t.nodeCount(), 0u);
    EXPECT_EQ(t.diagnostics().errorCount(), 0u);
}

// ── LexemeMeaning::validScopes per-meaning filter ──────────────────────

TEST(TreeBuilder, PerMeaningValidScopesFiltersResolution) {
    // A `%` with one meaning declared as `validScopes: ["Block"]` — it
    // should resolve only when Block is on the active scope stack.
    constexpr std::string_view cfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "VS", "version": "0.1.0" },
      "tokens": {
        "%": [{ "kind": "BlockOnlyOperator", "validScopes": ["Block"] }]
      },
      "shapes": { "root": { "sequence": ["Identifier"] } }
    })JSON";
    auto schema = *GrammarSchema::loadFromText(cfg);
    auto src    = SourceBuffer::fromString("%", "<vs>");

    Token pct{
        .coreKind   = CoreTokenKind::Operator,
        .schemaKind = InvalidSchemaToken,
        .span       = SourceSpan::of(0, 1),
    };

    // Without Block on the stack: no meaning matches → P_UnknownToken.
    {
        TreeBuilder b{src, schema};
        auto root = b.open(schema->rules().find("root"));
        b.pushToken(pct);
        root.close();
        Tree t = std::move(b).finish();
        EXPECT_EQ(countCode(t.diagnostics().all(),
                            DiagnosticCode::P_UnknownToken), 1u);
    }
    // With Block on the stack: meaning matches → no diagnostic.
    {
        TreeBuilder b{src, schema};
        auto root = b.open(schema->rules().find("root"));
        b.pushScope(ScopeKind::Block);
        b.pushToken(pct);
        b.popScope();
        root.close();
        Tree t = std::move(b).finish();
        EXPECT_EQ(t.diagnostics().errorCount(), 0u);
        // Single leaf attached.
        auto kids = t.children(t.root());
        ASSERT_EQ(kids.size(), 1u);
        EXPECT_EQ(t.kind(kids[0]), NodeKind::Token);
    }
}
