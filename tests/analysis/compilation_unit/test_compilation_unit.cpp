// CU1 + CU2 contract tests for `CompilationUnit` + `UnitBuilder`. The shape
// pinned here is what every later CU PR + the semantic phase consumes,
// so a regression here means the substrate changed in a way that breaks
// downstream phases — not just a local bug.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/e2e_harness.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using namespace dss;
using dss::tests::tokenizeShipped;

// Parse `source` against the shipped `toy` schema and return the resulting
// Tree. The lexer-diag check on E2EHarness is dismissed because this helper
// builds a Tree directly (CU1-style addTree path) and doesn't inspect them.
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

// True if any diagnostic in `rep` carries `code`.
[[nodiscard]] bool hasCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    auto all = rep.all();
    return std::any_of(all.begin(), all.end(),
                       [code](ParseDiagnostic const& d) { return d.code == code; });
}

// RAII temp file: writes `content` to a uniquely-named file in the temp dir
// and removes it on destruction. Used by addFile tests.
class TempFile {
public:
    explicit TempFile(std::string content) {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("dss_cu2_" + std::to_string(counter.fetch_add(1)) + ".toy");
        std::ofstream(path_, std::ios::binary) << content;
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFile(TempFile const&)            = delete;
    TempFile& operator=(TempFile const&) = delete;

    [[nodiscard]] std::filesystem::path const& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

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

TEST(CompilationUnit, CrossRefsEmptyForToy) {
    // CU4 landed: the ImportResolver now populates crossRefs for c-subset
    // (#include) and tsql-subset (table refs). toy is the identity resolver
    // (no import syntax), so a toy CU still has zero cross-refs — this remains
    // a valid, intentional assertion. Cross-language population is exercised in
    // test_import_resolver.cpp.
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

// ── CU2: addFile / addInMemory ────────────────────────────────────────────

TEST(CompilationUnitCU2, AddInMemoryParsesAndAdds) {
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addInMemory("var x = y;", "<mem>");   // identifier RHS — clean toy program
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(cu.trees()[0].id().valid());
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());   // clean parse
    EXPECT_TRUE(cu.driverDiagnostics().all().empty());        // no driver errors
}

TEST(CompilationUnitCU2, AddMultipleInMemoryPreservesOrder) {
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addInMemory("var a = 1;", "a.toy");
    b.addInMemory("var b = 2;", "b.toy");
    b.addInMemory("var c = 3;", "c.toy");
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 3u);
    EXPECT_LT(cu.trees()[0].id().v, cu.trees()[1].id().v);
    EXPECT_LT(cu.trees()[1].id().v, cu.trees()[2].id().v);
}

TEST(CompilationUnitCU2, AddInMemoryEmptyEmitsInfoButStillAdds) {
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addInMemory("", "<empty>");
    auto cu = std::move(b).finish();

    // Empty translation unit is valid: the (empty) tree is still added,
    // with a non-fatal D_EmptyInput note in the driver diagnostics.
    EXPECT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_EmptyInput));
}

TEST(CompilationUnitCU2, LexerDiagnosticsMergedIntoTree) {
    // Q1=C: the tokenizer's lexer diagnostics are folded into the produced
    // Tree's reporter. `@` is an illegal character — the tokenizer emits a
    // lexer-level P_IllegalChar (see test_parser_toy.cpp). Without the merge
    // that diagnostic lives only in the dropped tokenizer reporter; with the
    // merge it is visible via tree.diagnostics(). (The parser also emits
    // P_NoAlternativeMatched here; we assert specifically on the LEXER code.)
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addInMemory("var x = @;", "<mem>");
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(hasCode(cu.trees()[0].diagnostics(),
                        DiagnosticCode::P_IllegalChar))
        << "lexer diagnostic (P_IllegalChar) was not merged into the Tree "
           "(Q1=C regression)";
}

TEST(CompilationUnitCU2, AddFileReadsParsesAndAdds) {
    TempFile f{"var x = y;"};
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addFile(f.path());
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_TRUE(cu.driverDiagnostics().all().empty());
}

TEST(CompilationUnitCU2, AddFileMissingEmitsFileNotFoundAndContinues) {
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addFile("definitely/does/not/exist_a8f3.toy");   // → D_FileNotFound, skip
    b.addInMemory("var ok = 1;", "<mem>");             // continue: still added
    auto cu = std::move(b).finish();

    EXPECT_EQ(cu.trees().size(), 1u);   // only the good one
    EXPECT_TRUE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_FileNotFound));
}

TEST(CompilationUnitCU2, AddFileDuplicateEmitsWarningAndSkips) {
    TempFile f{"var x = y;"};
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addFile(f.path());
    b.addFile(f.path());   // same canonical path → skipped
    auto cu = std::move(b).finish();

    EXPECT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_DuplicateFile));
}

TEST(CompilationUnitCU2, AddFileDuplicateViaDifferentSpellingSkips) {
    // Proves dedup keys on the weakly-canonical path, not the raw string:
    // the same file reached via a `/./` detour must still be detected.
    TempFile f{"var x = y;"};
    auto alt = f.path().parent_path() / "." / f.path().filename();
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addFile(f.path());
    b.addFile(alt);        // different spelling, same canonical target
    auto cu = std::move(b).finish();

    EXPECT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_DuplicateFile));
}

TEST(CompilationUnitCU2, AddInMemorySameLabelTwiceAddsTwo) {
    // addInMemory does NOT dedup (labels may legitimately repeat) — both
    // sources are parsed and added; no D_DuplicateFile.
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addInMemory("var x = y;", "dup");
    b.addInMemory("var z = y;", "dup");
    auto cu = std::move(b).finish();

    EXPECT_EQ(cu.trees().size(), 2u);
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_DuplicateFile));
}

TEST(CompilationUnitCU2, MixedFileAndInMemoryPreservesOrder) {
    // addFile and addInMemory share parseAndAdd_/addTree; interleaving them
    // preserves add-order (strictly increasing TreeIds).
    TempFile f{"var b = y;"};
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addInMemory("var a = y;", "a.toy");
    b.addFile(f.path());
    b.addInMemory("var c = y;", "c.toy");
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 3u);
    EXPECT_LT(cu.trees()[0].id().v, cu.trees()[1].id().v);
    EXPECT_LT(cu.trees()[1].id().v, cu.trees()[2].id().v);
}

TEST(CompilationUnitCU2DeathTest, AddInMemoryAfterFinishAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    [[maybe_unused]] auto cu = std::move(b).finish();
    EXPECT_DEATH({ b.addInMemory("var x = y;", "<mem>"); },
                 "addInMemory.*called after finish");
}

TEST(CompilationUnitCU2DeathTest, AddFileAfterFinishAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    [[maybe_unused]] auto cu = std::move(b).finish();
    // Exercises the addFile guard specifically (distinct from addInMemory).
    EXPECT_DEATH({ b.addFile("anything.toy"); },
                 "addFile.*called after finish");
}
