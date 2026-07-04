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
#include <cstdint>
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
    b.addTree(parseToyTree("var x : int = 1;"));
    auto cu = std::move(b).finish();

    EXPECT_TRUE(cu.id().valid());
    EXPECT_EQ(cu.id(), builderId);            // id stable from builder ctor to finish (L2)
    EXPECT_EQ(&cu.schema(), schemaRaw);       // homogeneous-case schema pointer equality
    EXPECT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(cu.crossRefs().empty());      // D4: populated in CU4
    EXPECT_EQ(cu.driverDiagnostics().all().size(), 0u);  // D2: D_* codes start in CU2
}

// c105 (D-PP-USER-DEFINE, the audit-F3 pin): `--define` macros on a language
// WITHOUT a preprocess block (toy) can never be consumed — the plain
// tokenize→parse path must fail LOUD (D_DefineRequiresPreprocess), never
// silently ignore the user's macros. RED-ON-DISABLE: drop the guard in
// parseAndAdd_'s non-preprocess arm → zero driver diagnostics → this fails.
TEST(CompilationUnit, UserDefinesWithoutPreprocessBlockFailLoud) {
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.setUserDefines({"FOO=1"});
    b.addInMemory("var x : int = 1;", "<mem>");
    auto cu = std::move(b).finish();
    std::size_t hits = 0;
    for (auto const& d : cu.driverDiagnostics().all()) {
        if (d.code == DiagnosticCode::D_DefineRequiresPreprocess) ++hits;
    }
    EXPECT_EQ(hits, 1u)
        << "one non-preprocess file + pending --define macros must emit "
           "exactly one D_DefineRequiresPreprocess";
}

TEST(CompilationUnit, BuildMultipleTreesPreservesOrder) {
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addTree(parseToyTree("var a : int = 1;"));
    b.addTree(parseToyTree("var b : int = 2;"));
    b.addTree(parseToyTree("var c : int = 3;"));
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
    b.addTree(parseToyTree("var x : int = 1;"));
    auto cu = std::move(b).finish();
    auto const originalId = cu.id();

    CompilationUnit moved{std::move(cu)};
    EXPECT_EQ(moved.id(), originalId);
    EXPECT_EQ(moved.trees().size(), 1u);
}

TEST(CompilationUnit, MoveAssignmentTransfersOwnership) {
    auto schema = loadToySchema();
    UnitBuilder b1{schema};
    b1.addTree(parseToyTree("var a : int = 1;"));
    auto cu1 = std::move(b1).finish();

    UnitBuilder b2{schema};
    b2.addTree(parseToyTree("var b : int = 2;"));
    b2.addTree(parseToyTree("var c : int = 3;"));
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
    b.addTree(parseToyTree("var x : int = 1;"));
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
    EXPECT_DEATH({ b.addTree(parseToyTree("var x : int = 1;")); },
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
    b.addTree(parseToyTree("var x : int = 1;"));
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
    b.addInMemory("var x : int = y;", "<mem>");   // identifier RHS — clean toy program
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(cu.trees()[0].id().valid());
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());   // clean parse
    EXPECT_TRUE(cu.driverDiagnostics().all().empty());        // no driver errors
}

TEST(CompilationUnitCU2, AddMultipleInMemoryPreservesOrder) {
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addInMemory("var a : int = 1;", "a.toy");
    b.addInMemory("var b : int = 2;", "b.toy");
    b.addInMemory("var c : int = 3;", "c.toy");
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
    b.addInMemory("var x : int = @;", "<mem>");
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(hasCode(cu.trees()[0].diagnostics(),
                        DiagnosticCode::P_IllegalChar))
        << "lexer diagnostic (P_IllegalChar) was not merged into the Tree "
           "(Q1=C regression)";
}

TEST(CompilationUnitCU2, AddFileReadsParsesAndAdds) {
    TempFile f{"var x : int = y;"};
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
    b.addInMemory("var ok : int = 1;", "<mem>");             // continue: still added
    auto cu = std::move(b).finish();

    EXPECT_EQ(cu.trees().size(), 1u);   // only the good one
    EXPECT_TRUE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_FileNotFound));
}

TEST(CompilationUnitCU2, AddFileDuplicateEmitsWarningAndSkips) {
    TempFile f{"var x : int = y;"};
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
    TempFile f{"var x : int = y;"};
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
    b.addInMemory("var x : int = y;", "dup");
    b.addInMemory("var z : int = y;", "dup");
    auto cu = std::move(b).finish();

    EXPECT_EQ(cu.trees().size(), 2u);
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_DuplicateFile));
}

TEST(CompilationUnitCU2, MixedFileAndInMemoryPreservesOrder) {
    // addFile and addInMemory share parseAndAdd_/addTree; interleaving them
    // preserves add-order (strictly increasing TreeIds).
    TempFile f{"var b : int = y;"};
    auto schema = loadToySchema();
    UnitBuilder b{schema};
    b.addInMemory("var a : int = y;", "a.toy");
    b.addFile(f.path());
    b.addInMemory("var c : int = y;", "c.toy");
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
    EXPECT_DEATH({ b.addInMemory("var x : int = y;", "<mem>"); },
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

// ── FC13 D-PP-FATAL-HALTS-PARSE: parser gated on a FATAL PP truncation ──

namespace {
[[nodiscard]] std::shared_ptr<GrammarSchema const> loadCSubsetSchema() {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) {
        ADD_FAILURE() << "loadShipped(\"c-subset\") failed";
        std::abort();
    }
    return *loaded;
}
} // namespace

// A >256-deep nested macro argument trips the preprocessor's macro-
// expansion-nesting backstop, which TRUNCATES the stream. The CU must
// GATE the parser (parse an EOF-only stream) so the run halts cleanly:
// the PP diagnostic surfaces, the parse does NOT crash / hang / cascade.
// Pre-fix this fed the truncated deep-paren stream to the parser and
// drove the expression recursion into a stack overflow / hang.
TEST(CompilationUnitPPGate, FatalMacroNestingTruncationGatesParser) {
    auto schema = loadCSubsetSchema();
    ASSERT_TRUE(schema->preprocess().enabled);

    // F(F(F(...F(0)...))) nested 300 deep > the PP's 256 backstop.
    std::string src = "#define F(x) (x)\nint v = ";
    constexpr int kNest = 300;
    for (int i = 0; i < kNest; ++i) src += "F(";
    src += "0";
    for (int i = 0; i < kNest; ++i) src += ")";
    src += ";\n";

    UnitBuilder b{schema};
    b.addInMemory(std::move(src), "<deep-macro>");
    auto cu = std::move(b).finish();

    // A tree was produced (no crash) and it carries the PP backstop
    // diagnostic. The backstop emits P_PreprocessorUnsupported.
    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(hasCode(cu.trees()[0].diagnostics(),
                        DiagnosticCode::P_PreprocessorUnsupported))
        << "the macro-nesting backstop diagnostic must surface on the tree";
    // NO expression-depth cascade: the parser was gated, so it never
    // walked the truncated deep-paren remainder. (Pre-fix the truncated
    // stream drove the expression recursion to its guard or a crash.)
    EXPECT_FALSE(hasCode(cu.trees()[0].diagnostics(),
                         DiagnosticCode::P_ExpressionTooDeep))
        << "a gated parse must not emit a secondary expression-depth cascade";
}

// CONTROL (non-error path unchanged): an ordinary object-macro compile
// with NO fatal PP error parses normally — the macro expands and the
// program is structurally present (gate did NOT fire). RED-on-disable
// for an over-broad gate: if the gate keyed on `hasErrors()` (or any
// recoverable diagnostic) this clean compile would still parse, but an
// unresolved-include sibling case (below) would wrongly gate.
TEST(CompilationUnitPPGate, OrdinaryMacroCompileIsNotGated) {
    auto schema = loadCSubsetSchema();
    UnitBuilder b{schema};
    b.addInMemory("#define Z 0\nint main(void){ return Z; }\n", "<ok>");
    auto cu = std::move(b).finish();
    ASSERT_EQ(cu.trees().size(), 1u);
    // Clean: no PP fatal, no expression-depth diagnostic, and the tree is
    // a real parse (root present, function decl reachable).
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_NE(cu.trees()[0].root(), InvalidNode);
}

// An UNRESOLVED `#include` is a RECOVERABLE PP error (the stream is
// intact — the missing header just contributes nothing). The gate must
// NOT fire: the rest of the file MUST still parse so its declarations
// surface. RED-on-disable for the over-broad `hasErrors()` gate (which
// would swallow the whole file on the include error).
TEST(CompilationUnitPPGate, UnresolvedIncludeStillParsesRestOfFile) {
    auto schema = loadCSubsetSchema();
    UnitBuilder b{schema};
    b.addInMemory("#include \"nonexistent_zzz.h\"\nint f(void){ return 0; }\n",
                  "<missing-inc>");
    auto cu = std::move(b).finish();
    ASSERT_EQ(cu.trees().size(), 1u);
    // The include error surfaced...
    EXPECT_TRUE(hasCode(cu.trees()[0].diagnostics(),
                        DiagnosticCode::P_PreprocessorIncludeError));
    // ...but the parser STILL ran: the `f` function declaration is in the
    // tree (gated-to-EOF would have dropped it).
    const auto fnRule = cu.trees()[0].schema().rules().find("topLevelDecl");
    bool sawFunc = false;
    for (std::uint32_t i = 1; i < cu.trees()[0].nodeCount(); ++i) {
        const NodeId id{i};
        if (cu.trees()[0].kind(id) == NodeKind::Internal &&
            fnRule.valid() && cu.trees()[0].rule(id).v == fnRule.v) {
            sawFunc = true;
            break;
        }
    }
    EXPECT_TRUE(sawFunc)
        << "a recoverable PP error must NOT gate the parse — the rest of "
           "the file must still parse";
}
