// ===========================================================================
// P69 L2b2 (lane `lm`, the L2b1 review's W1, P1 and D1) — THE BINDER SKETCH'S STRUCTURE.
//
// THE PROPERTY THIS FILE OWNS: the sketch's record is exact by construction —
// a name's chain holds its LIVE bindings only (a scope's close unlinks its own,
// and a lookup that meets a dead head aborts by name), a declarator-end binding
// is taken back by its declaring token's NODE, and snapshots restore last in
// first out (an out-of-order restore aborts by name). Every scenario that can
// abort runs in a child process (`EXPECT_EXIT`), so a broken structure reddens
// its own case with the sketch's message and the runner survives it: the cost
// pins (test_parser_speculation_refusal_cost.cpp) read the same sketch through
// its work counter, and every parse would abort under a broken close.
//
// ── RED-ON-DISABLE ─────────────────────────────────────────────────────────
//   * a scope's close that leaves its bindings on their chains → the first
//     case's child aborts "a scope's close did not unlink it", and so does the
//     second's, at its last lookup;
//   * the declarator-end note keyed by a value two declarations share (a source
//     offset, as it was) → the second case binds the outer name twice;
//   * a restore that accepts a snapshot other than the innermost live one → the
//     death case survives.
// ===========================================================================

#include "analysis/syntactic/binder_sketch.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/strong_ids.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::shared_ptr<GrammarSchema const> cSchema() {
    auto loaded = GrammarSchema::loadShipped("c");
    EXPECT_TRUE(loaded.has_value()) << "the shipped c language document";
    return loaded.has_value() ? *loaded : nullptr;
}

// A child's verdict: 0 when every answer was right, 1 (with the first wrong one on stderr) otherwise.
[[noreturn]] void endChild(char const* wrong) {
    if (wrong != nullptr) {
        std::fputs(wrong, stderr);
        std::fputc('\n', stderr);
        std::exit(1);
    }
    std::exit(0);
}

}  // namespace

// Every way a binding dies — its own block closing, a type floated past a declarator-dominator to a
// block that closes, an enumeration constant lifted past a composite body to a block that closes, a
// definition's parameter list closing and its copies' scope closing — inside and outside live probes
// that commit and that roll back; after each, a lookup answers the binding outside the dead one.
TEST(BinderSketchStructure, AClosedScopesBindingsAnswerNoLookup) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);
    RuleId const block     = schema->rules().find("block");
    RuleId const dominator = schema->rules().find("topLevelDecl");
    RuleId const composite = schema->rules().find("enumSpec");
    ASSERT_TRUE(block.valid() && dominator.valid() && composite.valid());
    EXPECT_EXIT(
        {
            BinderSketch sk(*schema);
            using K = BinderSketch::NameKind;
            sk.record("T", /*isType=*/true);
            sk.record("V", /*isType=*/false);
            // (1) its own block, no probe
            sk.openScope(block);
            sk.record("T", /*isType=*/false);
            if (sk.lookup("T") != K::Value) endChild("a block's own binding did not answer");
            (void)sk.closeScope();
            if (sk.lookup("T") != K::Type) endChild("(1) a dead binding answered");
            // (2) a type floated past a dominator to a block, inside a probe that commits
            {
                BinderSketch::Snapshot committed = sk.snapshot();
                sk.openScope(block);
                sk.openScope(dominator);
                sk.record("V", /*isType=*/true);
                (void)sk.closeScope();
                if (sk.lookup("V") != K::Type) endChild("a floated type did not reach its block");
                (void)sk.closeScope();
                if (sk.lookup("V") != K::Value) endChild("(2) a dead binding answered");
            }
            // (3) an enumeration constant lifted past a composite, inside a probe that rolls back,
            // inside a live outer probe
            {
                BinderSketch::Snapshot outer = sk.snapshot();
                sk.openScope(block);
                {
                    BinderSketch::Snapshot inner = sk.snapshot();
                    sk.openScope(composite);
                    sk.record("T", /*isType=*/false, /*liftToEnclosingScope=*/true);
                    (void)sk.closeScope();
                    if (sk.lookup("T") != K::Value) endChild("a lifted name did not reach its block");
                    sk.restore(std::move(inner));
                }
                if (sk.lookup("T") != K::Type) endChild("(3) a rolled-back binding answered");
                sk.record("T", /*isType=*/false);
                (void)sk.closeScope();
                if (sk.lookup("T") != K::Type) endChild("(3) a dead binding answered");
                sk.restore(std::move(outer));
                if (sk.lookup("T") != K::Type) endChild("(3) after the outer rollback");
            }
            endChild(nullptr);
        },
        ::testing::ExitedWithCode(0), "");
}

// Two declarations of one name whose declaring tokens a macro gave ONE source offset — `#define
// DECL_T(i) int T = i` / `DECL_T(({ DECL_T(40); T + 1; }))`: the outer declarator ends (its name
// bound and noted), then the nested declaration ends and closes, then the outer row closes. Each
// close takes back its OWN note, so the outer name is bound once. Keyed by the offset, the nested
// close took the outer note back and the outer close bound the name a second time.
// In a child, as every case here that can abort: its last lookup follows a close, which a broken close turns
// into the sketch's abort (MEASURED 2026-11-16, macos-arm64-debug: run in-process, that abort ended the runner).
TEST(BinderSketchStructure, ADeclaratorsNameIsBoundOnceWhateverItsTokensShare) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);
    RuleId const block = schema->rules().find("block");
    ASSERT_TRUE(block.valid());
    EXPECT_EXIT(
        {
            BinderSketch sk(*schema);
            NodeId const outerName{7};
            NodeId const innerName{11};
            sk.recordAtDeclaratorEnd("T", /*isType=*/true, /*rowOwnsInnermostScope=*/false, outerName);
            sk.openScope(block);
            sk.recordAtDeclaratorEnd("T", /*isType=*/true, /*rowOwnsInnermostScope=*/false, innerName);
            // Each close takes back its own note and binds nothing: a binding would move the
            // sketch's `version()`, which every binding renews.
            std::uint64_t const beforeInner = sk.version();
            sk.recordUnlessBoundAtItsDeclarator("T", /*isType=*/true, innerName);
            if (sk.version() != beforeInner) endChild("the inner declaration's name was bound again at its close");
            (void)sk.closeScope();
            std::uint64_t const beforeOuter = sk.version();
            sk.recordUnlessBoundAtItsDeclarator("T", /*isType=*/true, outerName);
            if (sk.version() != beforeOuter) endChild("the outer declaration's name was bound again at its close");
            if (sk.lookup("T") != BinderSketch::NameKind::Type) endChild("the outer name did not answer as a type");
            endChild(nullptr);
        },
        ::testing::ExitedWithCode(0), "");
}

// Snapshots restore last in first out: the trails rewind exactly only in that order, and the
// parser's probes nest. A restore of an outer snapshot while an inner one lives is refused by name.
TEST(BinderSketchStructureDeath, ARestoreOfAnOuterSnapshotWhileAnInnerOneLivesAborts) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);
    EXPECT_DEATH(
        {
            BinderSketch sk(*schema);
            BinderSketch::Snapshot outer = sk.snapshot();
            BinderSketch::Snapshot inner = sk.snapshot();
            sk.restore(std::move(outer));
        },
        "not the innermost live one");
}
