// ===========================================================================
// P69 L2b2 (lane `lm`) — THE SCOPE CHAIN HOLDS WHAT CAN BE READ, NOT EVERY SCOPE
// EVER PUSHED.
//
// The tree builder's scope chain (scope_trail.hpp) held one node per scope push
// the parse ever made: ✔MEASURED 149,187 nodes at the end of sqlite3.c, committed
// and rolled back alike, where the stack it mirrors is as deep as the nesting. A
// node now goes when nothing can read it:
//   * at a rollback, every node pushed since the checkpoint — unless a trail
//     handed out (`scopeStack()`) reads it;
//   * at a pop, the popped node, when it is the newest, no kept diagnostic's trail
//     reads it (the captured watermark), no handed-out trail reads it (the held
//     watermark), and no live checkpoint can restore a stack entry naming it.
// A dropped node's id is taken by the next push under a NEW serial, and every
// trail checks the serials it reads: a trail that outlived its node is FATAL,
// never a rendering of another scope's kinds.
//
// ⚠ WHY THIS IS ITS OWN BINARY. Every arm of the rule above makes some trail read a
// reused node, and the serial check then ABORTS the run, which leaves no report to
// read the red from. So each case here asserts the chain's size (ASSERT, which
// returns) BEFORE any read or `finish` a defective drop would reach, and the
// binary holds nothing else. tests/core/test_tree_builder.cpp keeps the trails'
// own pins.
//
// RED-ON-DISABLE (tests/mutations/arms.registry), each case's own line says which.
// ===========================================================================

#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/scope_trail.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"
#include "core/types/tree_node.hpp"
#include "toy_harness.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::tests::ToyHarness;

namespace {

// A toy language with no `@`: an `@` raises a diagnostic under the scopes of the moment.
constexpr std::string_view kToyConfig = R"JSON({
  "dssSchemaVersion": 1,
  "language": { "name": "ScopeChainToy", "version": "0.1.0", "fileExtensions": [".t"] },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    ";":  [{ "kind": "EndCommand" }]
  },
  "shapes": {
    "root":       { "sequence": [{ "repeat": "statement" }] },
    "statement":  { "sequence": ["Identifier", "EndCommand"] }
  }
})JSON";

[[nodiscard]] ToyHarness harness(std::string text) { return ToyHarness::make(std::move(text), kToyConfig); }

[[nodiscard]] std::size_t countCode(std::span<ParseDiagnostic const> all, DiagnosticCode code) {
    return static_cast<std::size_t>(
        std::ranges::count_if(all, [code](ParseDiagnostic const& d) { return d.code == code; }));
}

} // namespace

// RED-ON-DISABLE: a pop that drops nothing — the chain grows by one per push.
TEST(ScopeChainMarks, TheChainHoldsOnlyTheLiveStackWhenNoTrailReadsIt) {
    auto h = harness("@");
    TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault()};
    auto root = b.open(h.schema->rules().find("root"));
    b.pushScope(ScopeKind::Block);
    for (int i = 0; i < 1000; ++i) {
        b.pushScope(ScopeKind::Paren);
        b.pushScope(ScopeKind::Bracket);
        ASSERT_EQ(b.scopeChainSize(), 3u) << "at push " << i;
        b.popScope();
        b.popScope();
        ASSERT_EQ(b.scopeChainSize(), 1u) << "a popped scope nothing reads kept its node, at pop " << i;
    }
    b.popScope();
    EXPECT_EQ(b.scopeChainSize(), 0u);
}

// A probe's branch pushes, raises a diagnostic and is rolled back, a thousand times: its nodes and its
// diagnostics' watermark go with it.
// RED-ON-DISABLE: a pop that drops nothing (the scope pushed after the loop stays); a pop that ignores
// the captured watermark (the kept diagnostic's scope goes with its pop); a rollback that keeps the
// branch's nodes, or the branch's watermark.
TEST(ScopeChainMarks, ARolledBackBranchLeavesNoScopeNode) {
    auto h = harness("@ @");
    TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault()};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushScope(ScopeKind::Block);
        for (int i = 0; i < 1000; ++i) {
            auto cp = b.checkpoint();
            b.pushScope(ScopeKind::Paren);
            b.pushToken(h.tok("@", 0, CoreTokenKind::Operator));   // raised under Block > Paren
            b.popScope();
            b.pushScope(ScopeKind::Bracket);
            b.popScope();
            b.rollback(std::move(cp));
            ASSERT_EQ(b.scopeChainSize(), 1u) << "the refuted branch's scope nodes survived rollback " << i;
        }
        b.pushScope(ScopeKind::Generic);
        b.popScope();
        ASSERT_EQ(b.scopeChainSize(), 1u) << "a scope popped after the refuted branches kept its node";
        b.pushToken(h.tok("@", 2, CoreTokenKind::Operator));       // kept, under Block
        b.popScope();
        ASSERT_EQ(b.scopeChainSize(), 1u) << "the kept diagnostic's scope went with its pop";
    }
    Tree t = std::move(b).finish();
    ASSERT_EQ(countCode(t.diagnostics().all(), DiagnosticCode::P_UnknownToken), 1u);
    auto const kept = std::ranges::find_if(t.diagnostics().all(), [](ParseDiagnostic const& d) {
        return d.code == DiagnosticCode::P_UnknownToken;
    });
    EXPECT_EQ(kept->scopeStack.kinds(), std::vector<ScopeKind>{ScopeKind::Block});
}

// A kept diagnostic's scope keeps its node through the pop of that scope and every later push.
// RED-ON-DISABLE: a pop that ignores the captured watermark.
TEST(ScopeChainMarks, AKeptDiagnosticsScopesKeepTheirNodesThroughTheirPops) {
    auto h = harness("@ @");
    TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault()};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushScope(ScopeKind::Block);
        b.pushScope(ScopeKind::Paren);
        b.pushToken(h.tok("@", 0, CoreTokenKind::Operator));   // kept: Block > Paren
        b.popScope();
        ASSERT_GE(b.scopeChainSize(), 2u) << "the popped scope a kept diagnostic reads lost its node";
        b.pushScope(ScopeKind::Bracket);
        b.pushToken(h.tok("@", 2, CoreTokenKind::Operator));   // kept: Block > Bracket
        b.popScope();
        b.popScope();
        ASSERT_EQ(b.scopeChainSize(), 3u) << "the kept diagnostics' scopes lost their nodes";
    }
    Tree t = std::move(b).finish();
    std::vector<std::vector<ScopeKind>> read;
    for (ParseDiagnostic const& d : t.diagnostics().all()) {
        if (d.code == DiagnosticCode::P_UnknownToken) read.push_back(d.scopeStack.kinds());
    }
    EXPECT_EQ(read, (std::vector<std::vector<ScopeKind>>{{ScopeKind::Block, ScopeKind::Paren},
                                                          {ScopeKind::Block, ScopeKind::Bracket}}));
}

// A stack's trail is its TOP's, which need not be the chain's newest node: the scope a kept diagnostic
// reads keeps its node after its pop, above the top.
// RED-ON-DISABLE: a trail whose top is the chain's newest node (it reads the popped scope); a pop that
// ignores the captured watermark (the node it asserts is gone).
TEST(ScopeChainMarks, AStacksTrailIsItsTopNotTheChainsNewestNode) {
    auto h = harness("@");
    TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault()};
    auto root = b.open(h.schema->rules().find("root"));
    b.pushScope(ScopeKind::Block);
    b.pushScope(ScopeKind::Paren);
    b.pushToken(h.tok("@", 0, CoreTokenKind::Operator));   // kept: Paren's node stays after its pop
    b.popScope();
    ASSERT_EQ(b.scopeChainSize(), 2u) << "the popped scope a kept diagnostic reads lost its node";
    EXPECT_EQ(b.scopeStack().kinds(), std::vector<ScopeKind>{ScopeKind::Block})
        << "the stack's trail is the chain's newest node, not its top";
    b.popScope();
}

// A trail handed out (`scopeStack()`) keeps the nodes it reads through every later pop and every later
// rollback, whoever holds it.
// RED-ON-DISABLE: a pop that ignores the held watermark; a rollback that truncates below it.
TEST(ScopeChainMarks, AHandedOutTrailKeepsItsNodesThroughPopsAndRollbacks) {
    auto h = harness("@");
    TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault()};
    auto root = b.open(h.schema->rules().find("root"));
    b.pushScope(ScopeKind::Block);
    b.pushScope(ScopeKind::Paren);
    ScopeTrail const popped = b.scopeStack();
    b.popScope();
    ASSERT_GE(b.scopeChainSize(), 2u) << "the popped scope a handed-out trail reads lost its node";
    b.pushScope(ScopeKind::Bracket);   // the next id, never the popped one's
    EXPECT_EQ(popped.kinds(), (std::vector<ScopeKind>{ScopeKind::Block, ScopeKind::Paren}));
    b.popScope();
    auto cp = b.checkpoint();
    b.pushScope(ScopeKind::Generic);
    ScopeTrail const refuted = b.scopeStack();
    b.rollback(std::move(cp));
    ASSERT_GE(b.scopeChainSize(), 3u) << "the rollback dropped the node a handed-out trail reads";
    b.pushScope(ScopeKind::Paren);
    EXPECT_EQ(refuted.kinds(), (std::vector<ScopeKind>{ScopeKind::Block, ScopeKind::Generic}));
    EXPECT_EQ(popped.kinds(), (std::vector<ScopeKind>{ScopeKind::Block, ScopeKind::Paren}));
    b.popScope();
    b.popScope();
}

// A scope pushed BEFORE a checkpoint and popped inside it is the one place a node must stay although no
// trail reads it: the rollback puts its stack entry back, naming it by id. The serial check cannot help
// there — the restored entry reads whatever node holds that id — so the pop's own rule is the guard.
// RED-ON-DISABLE: a pop that ignores the innermost checkpoint's chain size.
// Under it the case must report, never end the binary: a rollback (the checkpoint's own destructor included)
// would restore a stack entry naming a node the chain no longer holds, which the chain refuses fatally. So a
// lost node COMMITS the checkpoint before the case fails (MEASURED 2026-11-16: an ASSERT here left the
// checkpoint to its destructor, whose rollback ended the binary and the arm read unattributed).
TEST(ScopeChainMarks, AScopePoppedInsideACheckpointKeepsItsNodeForTheRollback) {
    auto h = harness("@");
    TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault()};
    auto root = b.open(h.schema->rules().find("root"));
    b.pushScope(ScopeKind::Block);
    b.pushScope(ScopeKind::Paren);
    auto cp = b.checkpoint();
    b.popScope();
    if (b.scopeChainSize() < 2u) {
        b.commit(std::move(cp));
        FAIL() << "a scope popped inside a checkpoint lost the node its rollback restores (the chain holds "
               << b.scopeChainSize() << ")";
    }
    b.pushScope(ScopeKind::Bracket);
    b.popScope();
    b.rollback(std::move(cp));
    EXPECT_EQ(b.scopeStack().kinds(), (std::vector<ScopeKind>{ScopeKind::Block, ScopeKind::Paren}))
        << "the rolled-back stack reads a scope of the refuted branch";
    b.popScope();
    b.popScope();
}

// The serial check itself: a trail whose node was dropped and whose id was taken again is fatal.
// RED-ON-DISABLE: the check removed — the trail reads {Block, Bracket} and the child exits 0.
TEST(ScopeChainMarksDeathTest, ATrailReadingANodeReusedAfterItWasTakenIsFatal) {
    EXPECT_DEATH(
        {
            auto                chain = std::make_shared<ScopeChain>();
            std::uint32_t const block = chain->push(ScopeKind::Block, ScopeChain::kNoNode);
            std::uint32_t const paren = chain->push(ScopeKind::Paren, block);
            ScopeTrail const    t     = ScopeTrail::at(chain, paren, 2);
            chain->dropNewest();
            (void)chain->push(ScopeKind::Bracket, block);   // paren's id, a new serial
            std::vector<ScopeKind> const read = t.kinds();
            std::fprintf(stderr, "read %zu kinds\n", read.size());
            std::exit(0);
        },
        "reads a node reused after the trail was taken");
}
