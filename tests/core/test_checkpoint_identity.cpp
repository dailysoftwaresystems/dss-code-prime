// ── SPECULATIVE ROLLBACK MUST BE INDISTINGUISHABLE FROM NEVER HAVING TRIED ──
//
// `test_checkpoint.cpp` asks whether each snapshotted AXIS comes back. This
// file asks the only question that actually protects the compiler: after a
// speculative branch is rolled back, is the tree the SAME TREE it would have
// been if the branch had never been attempted?
//
// ★ WHY IT IS A DIFFERENTIAL TEST AND NOT A LIST OF FIELD COMPARISONS. A
// checkpoint that restores ALMOST correctly is a silent-miscompile generator:
// the parse succeeds, the diagnostics look clean, and one node's parent link
// or one slot of the pending-children staging area carries a value from the
// abandoned branch. There is no way to enumerate "everything that could have
// leaked" — so each case here builds the SAME tree twice, once plainly and
// once through a speculative excursion that is rolled back, and compares the
// two FINISHED trees node for node: kind, flags, rule, token kind, span,
// parent, and child list, plus the diagnostic stream. Anything that leaks is
// a difference, whether or not anyone thought of it.
//
// ⚠ NodeIds carry the producing tree's TreeId tag, so two builders never mint
// equal ids. Everything here compares the id's INDEX (`.v`), which is the
// structural identity, and never the tagged id.
//
// ★ THE HOLES THIS FILE HAS CAUGHT ARE ALL ONE SHAPE: A WRITE IN PLACE AT AN
// INDEX BELOW THE CHECKPOINT'S MARK, UNDER A CONTAINER RESTORED BY SIZE.
// Two were live at the base commit and are ✔MEASURED red against it:
//   1. `pendingChildren_` was restored by SIZE. It SHRINKS whenever a frame
//      closes, so a branch that closed a pre-checkpoint frame and then
//      attached new children put the staging area back at the right LENGTH
//      holding the WRONG NODE IDS — ids the arena rollback had just
//      truncated away.
//   2. `wrapLastChildInFrame` writes the wrapped child's `parent` IN PLACE.
//      The wrapped child is the already-built left operand, which routinely
//      predates the checkpoint, so `arena.truncateTo` could not undo it: the
//      operand ended up parented to a wrapper node that no longer existed.
// The frame stack's own in-place write (`openerSpan` on the wrap) is the same
// shape one level up, and is NOT a third: at the base commit the checkpoint
// copied `open_` wholesale, so that write was already undone. It is listed
// because the trail rendition is what makes it a live edge, and case 2 covers
// it.
//   3. The DIAGNOSTIC REPORTER had the shape a third time, one layer DOWN, and
//      it survived the trail: `truncateTo` restores `all_` by SIZE while
//      `noteCapDrop_` / `rewriteElisionMarker_` rewrite a marker's `actual`
//      prose in place, at an index that predates the checkpoint whenever the
//      marker was minted before it. Nothing in the tree moved — the marker
//      simply stated the ABANDONED branch's elision count while the ledger
//      held the restored one, i.e. a rolled-back branch changing the number an
//      operator acts on. Case 7 below.
//
// ⚠ WHICH IS WHY `render` PRINTS `d.actual`. It used to print `code@span`
// only, and hole 3 is invisible from there — the marker keeps its code, its
// position and its place in the stream, and only its SENTENCE is wrong. A
// differential test is only as wide as its rendering.

#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/scope_kind.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

constexpr std::string_view kCfg = R"JSON({
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

struct Harness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
    RuleId                               root{};
    RuleId                               stmt{};
};

[[nodiscard]] Harness make(std::string source) {
    auto loaded = GrammarSchema::loadFromText(kCfg);
    EXPECT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    Harness h;
    h.src    = SourceBuffer::fromString(std::move(source), "<identity>");
    h.schema = loaded.has_value() ? *loaded : nullptr;
    if (h.schema) {
        h.root = h.schema->rules().find("root");
        h.stmt = h.schema->rules().find("stmt");
    }
    return h;
}

[[nodiscard]] Token tokAt(SourceBuffer const& src, std::string_view text,
                          CoreTokenKind kind = CoreTokenKind::Operator,
                          std::size_t hint = 0) {
    const auto sv    = src.text();
    const auto found = sv.find(text, hint);
    EXPECT_NE(found, std::string_view::npos)
        << "lexeme '" << text << "' not in source";
    return Token{
        .coreKind   = kind,
        .schemaKind = InvalidSchemaToken,
        .span       = SourceSpan::of(static_cast<ByteOffset>(found),
                                     static_cast<ByteOffset>(found + text.size())),
    };
}

// A flat, tree-id-independent rendering of one finished tree. Every field
// `Tree` exposes per node, plus the diagnostic stream, so a difference
// anywhere shows up as a difference here.
[[nodiscard]] std::string render(Tree const& t) {
    std::string out;
    out += "root=" + std::to_string(t.root().v)
         + " nodes=" + std::to_string(t.nodeCount()) + "\n";
    // Slot 0 is the arena's reserved sentinel, so real ids run
    // [1, nodeCount). An empty tree has neither.
    for (std::uint32_t i = 1;
         t.root().valid() && i < static_cast<std::uint32_t>(t.nodeCount()); ++i) {
        const NodeId id{i, t.root().arenaTag};
        out += "#" + std::to_string(i);
        out += " kind=" + std::to_string(static_cast<int>(t.kind(id)));
        out += " flags=" + std::to_string(
                   static_cast<std::uint32_t>(t.flags(id)));
        out += " span=" + std::to_string(
                   static_cast<std::uint64_t>(t.span(id).start()))
             + ":" + std::to_string(
                   static_cast<std::uint64_t>(t.span(id).end()));
        out += " parent=" + std::to_string(t.parent(id).v);
        if (t.kind(id) == NodeKind::Internal) {
            out += " rule=" + std::to_string(t.rule(id).v);
        } else if (t.kind(id) == NodeKind::Token) {
            out += " tok=" + std::to_string(t.tokenKind(id).v);
        }
        out += " kids=[";
        for (auto c : t.children(id)) out += std::to_string(c.v) + ",";
        out += "]\n";
    }
    out += "diags:\n";
    for (auto const& d : t.diagnostics().all()) {
        out += "  " + std::to_string(static_cast<int>(d.code))
             + "@" + std::to_string(static_cast<std::uint64_t>(d.span.start()))
             // The RENDERED TEXT, not just the code and position: the
             // reporter's cap and elision markers are rewritten IN PLACE and
             // a rollback that misses them leaves a marker with the right
             // code at the right offset stating the wrong count. See the
             // file header.
             + " actual=" + d.actual
             + "\n";
    }
    return out;
}

} // namespace

// ── 1. A rolled-back frame-and-token excursion changes nothing ───────────

TEST(CheckpointIdentity, RolledBackExcursionYieldsTheSameTree) {
    auto h = make("a;b;");
    ASSERT_NE(h.schema, nullptr);

    auto build = [&](bool speculate) {
        TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault()};
        auto root = b.open(h.root);
        {
            auto s = b.open(h.stmt);
            b.pushToken(tokAt(*h.src, "a", CoreTokenKind::Word));
            b.pushToken(tokAt(*h.src, ";"));
        }
        if (speculate) {
            auto cp = b.checkpoint();
            {
                auto s = b.open(h.stmt);
                b.pushScope(ScopeKind::Block);
                b.pushToken(tokAt(*h.src, "b", CoreTokenKind::Word));
                b.pushError(SourceSpan::of(2, 3));
                b.popScope();
            }
            b.rollback(std::move(cp));
        }
        {
            auto s = b.open(h.stmt);
            b.pushToken(tokAt(*h.src, "b", CoreTokenKind::Word));
            b.pushToken(tokAt(*h.src, ";", CoreTokenKind::Operator, 3));
        }
        root.close();
        return std::move(b).finish();
    };

    EXPECT_EQ(render(build(true)), render(build(false)))
        << "a rolled-back speculative branch left a trace in the tree";
}

// ── 2. Wrapping a PRE-checkpoint child inside the branch ─────────────────
//
// This is hole 2: `wrapLastChildInFrame` re-parents an already-built node in
// place, and that node is older than the checkpoint.

TEST(CheckpointIdentity, RolledBackWrapRestoresTheWrappedChildsParent) {
    auto h = make("a;b;");
    ASSERT_NE(h.schema, nullptr);

    auto build = [&](bool speculate) {
        TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault()};
        auto root = b.open(h.root);
        auto s    = b.open(h.stmt);
        b.pushToken(tokAt(*h.src, "a", CoreTokenKind::Word));

        if (speculate) {
            auto cp = b.checkpoint();
            {
                // Wraps the `a` token — a node that predates `cp`.
                auto w = b.wrapLastChildInFrame(h.stmt);
                b.pushToken(tokAt(*h.src, "b", CoreTokenKind::Word));
            }
            b.rollback(std::move(cp));
        }
        b.pushToken(tokAt(*h.src, ";"));
        s.close();
        root.close();
        Tree t = std::move(b).finish();
        return render(t);
    };

    EXPECT_EQ(build(true), build(false))
        << "a rolled-back wrap left the wrapped child parented to a node the "
           "rollback destroyed";
}

// ── 3. Inner commit, outer rollback ──────────────────────────────────────

TEST(CheckpointIdentity, InnerCommitThenOuterRollbackYieldsTheSameTree) {
    auto h = make("a;b;");
    ASSERT_NE(h.schema, nullptr);

    auto build = [&](bool speculate) {
        TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault()};
        auto root = b.open(h.root);
        {
            auto s = b.open(h.stmt);
            b.pushToken(tokAt(*h.src, "a", CoreTokenKind::Word));
            b.pushToken(tokAt(*h.src, ";"));
        }
        if (speculate) {
            auto outer = b.checkpoint();
            {
                auto inner = b.checkpoint();
                {
                    auto s = b.open(h.stmt);
                    b.pushToken(tokAt(*h.src, "b", CoreTokenKind::Word));
                }
                b.commit(std::move(inner));
            }
            {
                auto s = b.open(h.stmt);
                b.pushScope(ScopeKind::Block);
                b.pushToken(tokAt(*h.src, "b", CoreTokenKind::Word));
            }
            b.rollback(std::move(outer));
        }
        {
            auto s = b.open(h.stmt);
            b.pushToken(tokAt(*h.src, "b", CoreTokenKind::Word));
            b.pushToken(tokAt(*h.src, ";", CoreTokenKind::Operator, 3));
        }
        root.close();
        return render(std::move(b).finish());
    };

    EXPECT_EQ(build(true), build(false))
        << "an inner commit inside a rolled-back outer branch leaked";
}

// ── 4. Deep, repeated speculation ────────────────────────────────────────
//
// The shape the parser actually runs: many nested probes, all abandoned. It
// is here because a leak that is invisible at depth 2 (one stale slot the
// next push happens to overwrite with the same value) is not invisible at
// depth 400.

TEST(CheckpointIdentity, DeeplyNestedRolledBackProbesYieldTheSameTree) {
    auto h = make("a;b;");
    ASSERT_NE(h.schema, nullptr);
    constexpr int kDepth = 400;

    auto build = [&](bool speculate) {
        TreeBuilder b{h.src, h.schema,
                      DiagnosticBudget::libraryDefault(),
                      BuilderConfig{.maxSpeculationDepth = kDepth + 8}};
        auto root = b.open(h.root);
        std::vector<TreeBuilder::OpenScope> frames;
        for (int i = 0; i < kDepth; ++i) {
            frames.push_back(b.open(h.stmt));
            b.pushToken(tokAt(*h.src, "a", CoreTokenKind::Word));
        }
        if (speculate) {
            std::vector<TreeBuilder::Checkpoint> cps;
            std::vector<TreeBuilder::OpenScope>  probes;
            for (int i = 0; i < kDepth; ++i) {
                cps.push_back(b.checkpoint());
                probes.push_back(b.open(h.stmt));
                b.pushToken(tokAt(*h.src, "b", CoreTokenKind::Word));
                b.pushScope(ScopeKind::Block);
            }
            // Unwind innermost-out, the way SpeculationProbe does: drop the
            // frame guard first, then roll the checkpoint back.
            for (int i = kDepth - 1; i >= 0; --i) {
                probes.pop_back();
                b.rollback(std::move(cps[static_cast<std::size_t>(i)]));
            }
            cps.clear();
        }
        for (int i = kDepth - 1; i >= 0; --i) {
            b.pushToken(tokAt(*h.src, ";"));
            frames.pop_back();
        }
        root.close();
        return render(std::move(b).finish());
    };

    EXPECT_EQ(build(true), build(false))
        << kDepth << " nested rolled-back probes left a trace in the tree";
}

// ── 5. The scope stack survives a branch that unbalanced it ──────────────

TEST(CheckpointIdentity, RolledBackScopeChurnRestoresTheScopeStack) {
    auto h = make("a;b;");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault()};
    auto root = b.open(h.root);
    b.pushScope(ScopeKind::Block);
    b.pushScope(ScopeKind::Paren);
    const std::vector<ScopeKind> before(b.scopeStack().begin(), b.scopeStack().end());

    auto cp = b.checkpoint();
    b.popScope();
    b.popScope();                       // drains BELOW the checkpoint's depth
    b.pushScope(ScopeKind::Bracket);    // ... and refills the vacated slots
    b.pushScope(ScopeKind::Bracket);
    b.pushScope(ScopeKind::Generic);
    b.rollback(std::move(cp));

    const std::vector<ScopeKind> after(b.scopeStack().begin(), b.scopeStack().end());
    EXPECT_EQ(before, after)
        << "a branch that drained the scope stack below the checkpoint and "
           "refilled it left the wrong kinds behind";
    EXPECT_EQ(b.currentScope(), ScopeKind::Paren);

    root.close();
    Tree t = std::move(b).finish();
    (void)t;
}

// ── 6. Closing a PRE-checkpoint frame inside the branch ──────────────────
//
// This is hole 1, and it is LAST IN THE FILE ON PURPOSE. Against the
// pre-trail rendition it does not merely fail — the branch's node ids
// survive in the staging area, `finish()` flushes them, and the arena's own
// strong-id bounds contract fatal-aborts on an id the rollback truncated
// away. An abort takes the whole binary with it, so every case that ran
// AFTER it would lose its verdict. Putting it at the end means the mutant arm
// still reports the other five.
// The branch drives `pendingChildren_` BELOW the
// checkpoint's recorded size and then pushes new entries over the vacated
// slots, so a size-only restore hands back the right length holding the
// abandoned branch's node ids.

TEST(CheckpointIdentity, ClosingAnOuterFrameInsideTheBranchLeavesNoResidue) {
    auto h = make("a;b;");
    ASSERT_NE(h.schema, nullptr);

    auto build = [&](bool speculate) {
        TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault()};
        auto root = b.open(h.root);
        // `outer` is deliberately NOT closed by its guard on the
        // speculative arm — the branch closes it, the rollback re-opens
        // it, and `finish()` closes it synthetically. The control arm
        // must reach `finish()` with it open too, so neither arm closes
        // it here.
        auto outer = b.open(h.stmt);
        b.pushToken(tokAt(*h.src, "a", CoreTokenKind::Word));

        if (speculate) {
            auto cp = b.checkpoint();
            outer.close();                    // pendingChildren_ dips BELOW the mark
            {
                auto other = b.open(h.stmt);   // ... and refills the vacated slot
                b.pushToken(tokAt(*h.src, "b", CoreTokenKind::Word));
                b.pushToken(tokAt(*h.src, ";", CoreTokenKind::Operator, 3));
            }
            b.rollback(std::move(cp));
        }
        b.pushToken(tokAt(*h.src, ";"));       // lands in `outer`, still open
        Tree t = std::move(b).finish();
        return render(t);
    };

    EXPECT_EQ(build(true), build(false))
        << "the branch's node ids survived in the pending-children staging area";
}

// ── 7. A rolled-back ELISION must not leave its count in the marker ──────
//
// Hole 3, and it lives one layer below the builder: `DiagnosticReporter`
// restores `all_` by SIZE, but the cap marker and the per-code elision markers
// are rewritten IN PLACE at the index they were MINTED at. Mint the marker
// BEFORE the checkpoint, elide more inside the branch, and the rollback puts
// `elided_` back while the marker's sentence keeps the branch's number.
//
// Nothing structural leaks, which is exactly why it went unnoticed: the tree
// is identical, the diagnostic stream has the same length, the same codes and
// the same spans. The only thing wrong is the number an operator would act on
// — and "how many diagnostics am I not being shown" is not a cosmetic fact.
TEST(CheckpointIdentity, RolledBackElisionDoesNotLeaveItsCountInTheMarker) {
    auto h = make(std::string(32, 'a') + ";");
    ASSERT_NE(h.schema, nullptr);

    // A per-code cap of 2 so the THIRD diagnostic mints the marker, and no
    // dedup window so distinct spans never take the other elision route.
    DiagnosticReporter::Config cfg{};
    cfg.maxPerCode  = 2;
    cfg.dedupWindow = 0;
    const DiagnosticBudget budget{cfg};

    auto build = [&](bool speculate) {
        TreeBuilder b{h.src, h.schema, budget};
        auto root = b.open(h.root);
        auto s    = b.open(h.stmt);

        // Three of one code: two stored, the third coalesced — which MINTS
        // the elision marker at an index every checkpoint below now predates.
        for (std::uint32_t i = 0; i < 3; ++i) {
            b.pushError(SourceSpan::of(static_cast<ByteOffset>(i),
                                       static_cast<ByteOffset>(i + 1)));
        }
        if (speculate) {
            auto cp = b.checkpoint();
            // Five more of the same code inside the branch. Each one is
            // coalesced, so `all_` does not grow at all — the size restore
            // has nothing to do and the marker's prose is the ONLY thing the
            // branch changed.
            for (std::uint32_t i = 8; i < 13; ++i) {
                b.pushError(SourceSpan::of(static_cast<ByteOffset>(i),
                                           static_cast<ByteOffset>(i + 1)));
            }
            b.rollback(std::move(cp));
        }
        s.close();
        root.close();
        return render(std::move(b).finish());
    };

    EXPECT_EQ(build(true), build(false))
        << "a rolled-back branch left its own elision count standing in the "
           "marker's prose";
}
