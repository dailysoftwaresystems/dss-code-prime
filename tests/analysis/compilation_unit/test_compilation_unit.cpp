// CU1 contract tests for `CompilationUnit` + `UnitBuilder`. The shape
// pinned here is what every later CU PR + the semantic phase consumes,
// so a regression here means the substrate changed in a way that breaks
// downstream phases — not just a local bug.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/e2e_harness.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using namespace dss;
using dss::tests::tokenizeShipped;

// Parse `source` against the shipped `toy` schema and return the resulting
// Tree. The lexer-diag check on E2EHarness is dismissed because we don't
// inspect them here — the CU layer doesn't ingest tokenizer diagnostics in
// CU1 (CU2 will).
[[nodiscard]] Tree parseToyTree(std::string source) {
    auto h = tokenizeShipped("toy", std::move(source));
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    h.dismissLexerDiags();
    return std::move(result.tree);
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> loadToySchema() {
    auto loaded = GrammarSchema::loadShipped("toy");
    if (!loaded) {
        ADD_FAILURE() << "loadShipped(\"toy\") failed";
        std::abort();
    }
    return *loaded;
}

} // namespace

// ── happy path ────────────────────────────────────────────────────────────

TEST(CompilationUnit, BuildSingleTreeMatchesContract) {
    auto schema = loadToySchema();
    auto const* schemaRaw = schema.get();

    UnitBuilder b{schema};
    auto const builderId = b.id();
    b.addTree(parseToyTree("var x = 1;"));
    auto cu = std::move(b).finish();

    EXPECT_TRUE(cu.id().valid());
    EXPECT_EQ(cu.id(), builderId);            // id stable from builder ctor to finish (L2)
    EXPECT_EQ(&cu.schema(), schemaRaw);       // homogeneous-case schema pointer equality
    EXPECT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(cu.crossRefs().empty());      // D4: populated in CU4
    EXPECT_EQ(cu.driverDiagnostics().all().size(), 0u);  // D2: D_* codes start in CU2
}

TEST(CompilationUnit, BuildMultipleTreesPreservesOrder) {
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addTree(parseToyTree("var a = 1;"));
    b.addTree(parseToyTree("var b = 2;"));
    b.addTree(parseToyTree("var c = 3;"));
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 3u);
    // TreeBuilder mints TreeIds via a process-global monotonic counter, so
    // addTree-order Trees have strictly increasing TreeIds — that's what
    // pins "order preserved" structurally.
    EXPECT_LT(cu.trees()[0].id().v, cu.trees()[1].id().v);
    EXPECT_LT(cu.trees()[1].id().v, cu.trees()[2].id().v);
}

TEST(CompilationUnit, EmptyCompilationUnitIsValid) {
    // Per CU1 plan §2.3: "a single-tree CU is the smallest valid CU" — but
    // an empty CU is also valid (degenerate case for tests / driver hot-path).
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    auto cu = std::move(b).finish();

    EXPECT_TRUE(cu.id().valid());
    EXPECT_TRUE(cu.trees().empty());
    EXPECT_TRUE(cu.crossRefs().empty());
}

TEST(CompilationUnit, UniqueIdsAcrossBuilders) {
    // Each UnitBuilder mints its id at construction; three independent
    // builders must produce three distinct CUs. Specific id values are
    // process-state-dependent — assert distinctness, not values.
    auto schema = loadToySchema();
    UnitBuilder b1{schema};
    UnitBuilder b2{schema};
    UnitBuilder b3{schema};
    EXPECT_NE(b1.id(), b2.id());
    EXPECT_NE(b2.id(), b3.id());
    EXPECT_NE(b1.id(), b3.id());
}

TEST(CompilationUnit, NextIdIsMonotonic) {
    auto const a = CompilationUnit::nextId();
    auto const b = CompilationUnit::nextId();
    auto const c = CompilationUnit::nextId();
    EXPECT_LT(a.v, b.v);
    EXPECT_LT(b.v, c.v);
}

TEST(CompilationUnit, MoveTransfersOwnership) {
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addTree(parseToyTree("var x = 1;"));
    auto cu = std::move(b).finish();
    auto const originalId = cu.id();

    CompilationUnit moved{std::move(cu)};
    EXPECT_EQ(moved.id(), originalId);
    EXPECT_EQ(moved.trees().size(), 1u);
}

TEST(CompilationUnit, MoveAssignmentTransfersOwnership) {
    auto schema = loadToySchema();
    UnitBuilder b1{schema};
    b1.addTree(parseToyTree("var a = 1;"));
    auto cu1 = std::move(b1).finish();

    UnitBuilder b2{schema};
    b2.addTree(parseToyTree("var b = 2;"));
    b2.addTree(parseToyTree("var c = 3;"));
    auto cu2       = std::move(b2).finish();
    auto const cu2Id = cu2.id();

    cu1 = std::move(cu2);
    EXPECT_EQ(cu1.id(), cu2Id);
    EXPECT_EQ(cu1.trees().size(), 2u);  // 2 trees from cu2, not 1 from cu1
}

TEST(CompilationUnit, CrossRefsEmptyInCU1) {
    // LANDMARK(CU4): CU1 ships the CrossTreeRef struct + empty vector; CU4's
    // ImportResolver is what populates it. When that lands, this empty-span
    // assertion becomes wrong for multi-file CUs with imports — the CU4
    // author MUST revisit this test deliberately (don't just delete it).
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addTree(parseToyTree("var x = 1;"));
    auto cu = std::move(b).finish();
    EXPECT_TRUE(cu.crossRefs().empty());
}

TEST(CompilationUnit, TypeTraits) {
    // Static-asserted in a TEST body so the failure surfaces with the
    // test name (rather than an opaque "static_assert failed" build error).
    static_assert(!std::is_copy_constructible_v<CompilationUnit>);
    static_assert(!std::is_copy_assignable_v<CompilationUnit>);
    static_assert(std::is_move_constructible_v<CompilationUnit>);
    static_assert(std::is_move_assignable_v<CompilationUnit>);

    // UnitBuilder is single-use: explicitly non-copyable AND non-movable.
    // Mirrors Parser/TreeBuilder discipline; eliminates "moved-from vs
    // finished" ambiguity at the type level.
    static_assert(!std::is_copy_constructible_v<UnitBuilder>);
    static_assert(!std::is_move_constructible_v<UnitBuilder>);
}

// ── death tests (lifecycle invariants) ───────────────────────────────────
//
// These pin the runtime guards in compilation_unit.cpp. The SH3-analog
// cross-CU NodeId guard is deferred to CU3 (D3); CU1's death surface is
// only the builder's two-state lifecycle (active → finished).

TEST(CompilationUnitDeathTest, AddTreeAfterFinishAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    [[maybe_unused]] auto cu = std::move(b).finish();
    // `b` is non-movable; `std::move(b)` only casts to rvalue-ref. The
    // object's `finished_` latch is now true — addTree must abort.
    EXPECT_DEATH({ b.addTree(parseToyTree("var x = 1;")); },
                 "addTree.*called after finish");
}

TEST(CompilationUnitDeathTest, DoubleFinishAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    [[maybe_unused]] auto cu = std::move(b).finish();
    // A second rvalue-qualified finish() on the same live builder object
    // is syntactically valid (std::move doesn't consume); the latch fires.
    EXPECT_DEATH({ [[maybe_unused]] auto cu2 = std::move(b).finish(); },
                 "finish.*called twice");
}

TEST(CompilationUnitDeathTest, NullSchemaAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH({ UnitBuilder b{nullptr}; }, "schema is null");
}

TEST(CompilationUnitDeathTest, ReadingSchemaOnMovedFromCuAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addTree(parseToyTree("var x = 1;"));
    auto cu = std::move(b).finish();
    CompilationUnit moved{std::move(cu)};
    // `cu` is moved-from: its schema_ shared_ptr is null. Reading schema()
    // must abort, not dereference null. (trees()/crossRefs() on a moved-from
    // CU return empty spans and are intentionally safe — not tested for death.)
    EXPECT_DEATH({ (void)cu.schema(); }, "no schema");  // NOLINT(bugprone-use-after-move)
}
