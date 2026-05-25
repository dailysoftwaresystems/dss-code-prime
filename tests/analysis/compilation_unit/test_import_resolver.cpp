// CU4 tests for per-language import resolution — the ImportResolver populating
// CompilationUnit::crossRefs. Covers toy (identity), c-subset (#include
// following, recursion, cycles, unresolved), and tsql-subset (cross-statement
// table-name matching, table-vs-column position, unresolved).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_span.hpp"
#include "core/types/tree.hpp"

#include "analysis/compilation_unit/toy_cu_fixture.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace {

using namespace dss;
using dss::cu_test::countCode;
using dss::cu_test::hasCode;
using dss::cu_test::loadShippedSchema;

// RAII temp directory for the c-subset include tests: files must share a
// directory so same-directory `#include` resolution finds them.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("dss_cu4_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(dir_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    TempDir(TempDir const&)            = delete;
    TempDir& operator=(TempDir const&) = delete;

    std::filesystem::path write(std::string const& name, std::string const& content) const {
        auto path = dir_ / name;
        std::ofstream(path, std::ios::binary) << content;
        return path;
    }

    [[nodiscard]] std::filesystem::path const& path() const noexcept { return dir_; }

private:
    std::filesystem::path dir_;
};

} // namespace

// ── toy: identity ───────────────────────────────────────────────────────────

TEST(ImportResolver, ToyProducesNoCrossRefs) {
    UnitBuilder b{loadShippedSchema("toy")};
    b.addInMemory("var x = y;", "a.toy");
    b.addInMemory("var z = y;", "b.toy");
    auto cu = std::move(b).finish();
    EXPECT_TRUE(cu.crossRefs().empty());
}

// ── c-subset: #include following ─────────────────────────────────────────────

TEST(ImportResolver, CSubsetIncludeLoadsTargetAndLinksIt) {
    TempDir dir;
    auto main = dir.write("main.c", "#include \"helper.h\"\nint main() { return helper(); }\n");
    dir.write("helper.h", "int helper() { return 1; }\n");

    UnitBuilder b{loadShippedSchema("c-subset")};
    b.addFile(main);
    auto cu = std::move(b).finish();

    // helper.h was discovered + loaded by following the directive.
    ASSERT_EQ(cu.trees().size(), 2u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());   // main.c parses clean
    EXPECT_FALSE(cu.trees()[1].diagnostics().hasErrors());   // helper.h parses clean
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport));

    ASSERT_EQ(cu.crossRefs().size(), 1u);
    auto const& ref = cu.crossRefs()[0];
    Tree const& mainTree = cu.trees()[0];
    EXPECT_EQ(ref.sourceTree, mainTree.id());                // edge from main.c
    EXPECT_EQ(ref.targetTree, cu.trees()[1].id());           // to helper.h
    EXPECT_EQ(ref.targetNode, cu.trees()[1].root());         // targets the included tree root

    // sourceNode is the includeDirective node, and importSpan is exactly that
    // directive's span (which begins the file).
    ASSERT_TRUE(ref.sourceNode.valid());
    EXPECT_EQ(mainTree.kind(ref.sourceNode), NodeKind::Internal);
    EXPECT_EQ(mainTree.rule(ref.sourceNode), mainTree.rules().find("includeDirective"));
    ASSERT_TRUE(ref.importSpan.has_value());
    EXPECT_EQ(*ref.importSpan, mainTree.span(ref.sourceNode));
    EXPECT_EQ(ref.importSpan->start(), 0u);
}

TEST(ImportResolver, CSubsetTransitiveIncludeLoadsChain) {
    TempDir dir;
    auto a = dir.write("a.c", "#include \"b.h\"\nint a() { return 0; }\n");
    dir.write("b.h", "#include \"c.h\"\nint b() { return 0; }\n");
    dir.write("c.h", "int c() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addFile(a);
    auto cu = std::move(builder).finish();

    EXPECT_EQ(cu.trees().size(), 3u);        // a.c, b.h, c.h
    EXPECT_EQ(cu.crossRefs().size(), 2u);    // a->b, b->c
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport));
}

TEST(ImportResolver, CSubsetIncludeCycleTerminates) {
    TempDir dir;
    auto a = dir.write("a.h", "#include \"b.h\"\nint a() { return 0; }\n");
    dir.write("b.h", "#include \"a.h\"\nint b() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addFile(a);
    auto cu = std::move(builder).finish();

    // Each file loaded exactly once (dedup by canonical path breaks the cycle).
    EXPECT_EQ(cu.trees().size(), 2u);
    EXPECT_EQ(cu.crossRefs().size(), 2u);    // a->b and b->a, no infinite loop
}

TEST(ImportResolver, CSubsetMissingIncludeEmitsDiagnosticAndContinues) {
    TempDir dir;
    auto main = dir.write("main.c", "#include \"ghost.h\"\nint main() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addFile(main);
    auto cu = std::move(builder).finish();

    EXPECT_EQ(cu.trees().size(), 1u);                // only main.c — nothing loaded
    EXPECT_TRUE(cu.crossRefs().empty());             // no edge for an unresolved include
    EXPECT_EQ(countCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport), 1u);
}

TEST(ImportResolver, CSubsetIncludeDirResolvesAcrossDirectories) {
    TempDir srcDir;
    TempDir incDir;
    auto main = srcDir.write("main.c", "#include \"shared.h\"\nint main() { return 0; }\n");
    incDir.write("shared.h", "int shared() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addIncludeDir(incDir.path());   // declared include dir
    builder.addFile(main);
    auto cu = std::move(builder).finish();

    EXPECT_EQ(cu.trees().size(), 2u);
    ASSERT_EQ(cu.crossRefs().size(), 1u);
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport));
}

// ── tsql-subset: cross-statement table-name matching ─────────────────────────

TEST(ImportResolver, TsqlTableReferenceResolvesAcrossFiles) {
    UnitBuilder builder{loadShippedSchema("tsql-subset")};
    builder.addInMemory("CREATE TABLE Users (id INT, name VARCHAR);", "schema.sql");
    builder.addInMemory("SELECT name FROM Users;", "data.sql");
    auto cu = std::move(builder).finish();

    ASSERT_EQ(cu.crossRefs().size(), 1u);
    auto const& ref = cu.crossRefs()[0];
    EXPECT_EQ(ref.sourceTree, cu.trees()[1].id());   // data.sql references
    EXPECT_EQ(ref.targetTree, cu.trees()[0].id());   // schema.sql defines
    EXPECT_FALSE(ref.importSpan.has_value());         // name-matching has no directive span

    // Both endpoints point at qualifiedName nodes (the reference site and the
    // CREATE TABLE name), not roots or a swapped pair.
    auto const qualifiedName = [](Tree const& t) { return t.rules().find("qualifiedName"); };
    EXPECT_EQ(cu.trees()[1].rule(ref.sourceNode), qualifiedName(cu.trees()[1]));
    EXPECT_EQ(cu.trees()[0].rule(ref.targetNode), qualifiedName(cu.trees()[0]));

    // `name` is a column (operand) qualifiedName, not a table position — it
    // must NOT be flagged unresolved.
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedReference));
}

TEST(ImportResolver, TsqlQualifiedReferenceMatchesByTableName) {
    UnitBuilder builder{loadShippedSchema("tsql-subset")};
    builder.addInMemory("CREATE TABLE Orders (id INT);", "schema.sql");
    builder.addInMemory("DELETE FROM dbo.Orders;", "ops.sql");   // db-qualified ref
    auto cu = std::move(builder).finish();

    ASSERT_EQ(cu.crossRefs().size(), 1u);
    EXPECT_EQ(cu.crossRefs()[0].targetTree, cu.trees()[0].id());
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedReference));
}

TEST(ImportResolver, TsqlUnknownTableEmitsUnresolved) {
    UnitBuilder builder{loadShippedSchema("tsql-subset")};
    builder.addInMemory("SELECT * FROM Ghosts;", "q.sql");
    auto cu = std::move(builder).finish();

    EXPECT_TRUE(cu.crossRefs().empty());
    EXPECT_EQ(countCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedReference), 1u);
}

TEST(ImportResolver, TsqlSameFileReferenceIsNotACrossRef) {
    UnitBuilder builder{loadShippedSchema("tsql-subset")};
    builder.addInMemory("CREATE TABLE T (id INT); SELECT * FROM T;", "all.sql");
    auto cu = std::move(builder).finish();

    // T resolves within its own tree — intra-file, so no cross-tree edge and
    // no unresolved diagnostic.
    EXPECT_TRUE(cu.crossRefs().empty());
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedReference));
}

TEST(ImportResolver, TsqlInsertAndUpdateAreTablePositions) {
    UnitBuilder builder{loadShippedSchema("tsql-subset")};
    builder.addInMemory("CREATE TABLE Acct (id INT);", "schema.sql");
    builder.addInMemory("INSERT INTO Acct VALUES (1);", "ins.sql");
    builder.addInMemory("UPDATE Acct SET id = 2;", "upd.sql");
    auto cu = std::move(builder).finish();

    // Both the INSERT and the UPDATE table positions resolve to Acct.
    ASSERT_EQ(cu.crossRefs().size(), 2u);
    for (auto const& ref : cu.crossRefs()) {
        EXPECT_EQ(ref.targetTree, cu.trees()[0].id());
    }
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedReference));
}

// ── c-subset: dedup / edge cases ─────────────────────────────────────────────

TEST(ImportResolver, CSubsetEmptyIncludePathIsReported) {
    TempDir dir;
    // `#include ""` parses as a well-formed directive with an empty filename —
    // it resolves to nothing and must surface as D_UnresolvedImport, not a
    // silent skip.
    auto main = dir.write("main.c", "#include \"\"\nint main() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addFile(main);
    auto cu = std::move(builder).finish();

    EXPECT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(cu.crossRefs().empty());
    EXPECT_EQ(countCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport), 1u);
}

TEST(ImportResolver, CSubsetExplicitlyAddedIncludeTargetIsNotReloaded) {
    TempDir dir;
    auto main   = dir.write("main.c", "#include \"helper.h\"\nint main() { return 0; }\n");
    auto helper = dir.write("helper.h", "int helper() { return 1; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addFile(main);
    builder.addFile(helper);   // also added explicitly — must dedup, not double-load
    auto cu = std::move(builder).finish();

    EXPECT_EQ(cu.trees().size(), 2u);          // not 3 — the include reuses helper.h's tree
    ASSERT_EQ(cu.crossRefs().size(), 1u);
    EXPECT_EQ(cu.crossRefs()[0].targetTree, cu.trees()[1].id());
}

TEST(ImportResolver, CSubsetSharedHeaderLoadedOnceWithTwoEdges) {
    TempDir dir;
    auto a = dir.write("a.c", "#include \"common.h\"\nint a() { return 0; }\n");
    auto b = dir.write("b.c", "#include \"common.h\"\nint b() { return 0; }\n");
    dir.write("common.h", "int common() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addFile(a);
    builder.addFile(b);
    auto cu = std::move(builder).finish();

    EXPECT_EQ(cu.trees().size(), 3u);          // a.c, b.c, common.h (loaded once)
    ASSERT_EQ(cu.crossRefs().size(), 2u);      // a->common and b->common
    EXPECT_EQ(cu.crossRefs()[0].targetTree, cu.crossRefs()[1].targetTree);  // same header
    EXPECT_NE(cu.crossRefs()[0].sourceTree, cu.crossRefs()[1].sourceTree);  // distinct includers
}

TEST(ImportResolver, CSubsetInMemoryIncludeResolvesViaIncludeDir) {
    TempDir incDir;
    incDir.write("dep.h", "int dep() { return 0; }\n");

    // An in-memory source has a label, not a path, so same-directory resolution
    // can't apply — only declared include dirs can satisfy the #include.
    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addIncludeDir(incDir.path());
    builder.addInMemory("#include \"dep.h\"\nint main() { return 0; }\n", "main.c");
    auto cu = std::move(builder).finish();

    EXPECT_EQ(cu.trees().size(), 2u);
    EXPECT_EQ(cu.crossRefs().size(), 1u);
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport));
}

TEST(ImportResolver, CSubsetInMemoryIncludeWithoutIncludeDirIsUnresolved) {
    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addInMemory("#include \"dep.h\"\nint main() { return 0; }\n", "main.c");
    auto cu = std::move(builder).finish();

    EXPECT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(cu.crossRefs().empty());
    EXPECT_EQ(countCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport), 1u);
}
