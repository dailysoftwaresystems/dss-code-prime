// CU4 tests for per-language import resolution — the ImportResolver populating
// CompilationUnit::crossRefs. Covers toy (identity), c-subset (#include
// following, recursion, cycles, unresolved), and tsql-subset (cross-statement
// table-name matching, table-vs-column position, unresolved).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_span.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_visitor.hpp"

#include "analysis/compilation_unit/toy_cu_fixture.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
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

// Read the shipped `<name>.lang.json` TEXT by walking up from cwd to the repo
// `src/dss-config/sources/` directory — mirrors GrammarSchema::loadShipped's
// search so the genericity test reads the exact bytes the loader would.
[[nodiscard]] std::string readShippedConfigText(std::string_view name) {
    namespace fs = std::filesystem;
    std::string const leaf = std::string{name} + ".lang.json";
    std::error_code ec;
    fs::path here = fs::current_path(ec);
    for (int i = 0; i < 8 && !here.empty(); ++i) {
        fs::path const candidate = here / "src" / "dss-config" / "sources" / leaf;
        if (fs::exists(candidate, ec)) {
            std::ifstream in(candidate, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>());
        }
        fs::path const parent = here.parent_path();
        if (parent == here) break;
        here = parent;
    }
    ADD_FAILURE() << "could not locate shipped config '" << name << "'";
    std::abort();
}

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

TEST(ImportResolver, CSubsetQuoteIncludeIsInlinedByPreprocessor) {
    // FC13: a QUOTE `#include "h"` is now owned by the config-selected C
    // preprocessor, which splices the header TEXT into ONE synthesized buffer
    // BEFORE parsing (so a header `#define` could reach the includer). The
    // post-parse include-following arm is RETIRED for C quote includes: there
    // is exactly ONE tree (no separate header tree) and ZERO cross-refs (the
    // include is textual, not a cross-tree edge). The header's symbol is
    // physically present in the single tree's (synthesized) source.
    //
    // RED-ON-DISABLE: reverting the `if (schema->preprocess().enabled)` gate in
    // compilation_unit.cpp parseAndAdd_ makes the old post-parse arm run again
    // -> trees().size()==2 and crossRefs().size()==1, flipping these asserts.
    TempDir dir;
    auto main = dir.write("main.c", "#include \"helper.h\"\nint main() { return helper(); }\n");
    dir.write("helper.h", "int helper() { return 1; }\n");

    UnitBuilder b{loadShippedSchema("c-subset")};
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);              // header inlined, not a 2nd tree
    EXPECT_TRUE(cu.crossRefs().empty());           // textual include -> no edge
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport));
    // The header's declaration is physically spliced into the synthesized
    // source the single tree was parsed from.
    EXPECT_NE(std::string{cu.trees()[0].source().text()}.find("int helper()"),
              std::string::npos)
        << "preprocessor must splice the quote-included header text inline";
}

TEST(ImportResolver, CSubsetTransitiveQuoteIncludeInlinesChain) {
    // FC13: a quote include WITHIN a quote-included header is inlined
    // recursively into the SAME synthesized buffer -> still one tree, zero
    // cross-refs, and every transitively-included symbol present.
    TempDir dir;
    auto a = dir.write("a.c", "#include \"b.h\"\nint a() { return 0; }\n");
    dir.write("b.h", "#include \"c.h\"\nint b() { return 0; }\n");
    dir.write("c.h", "int c() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addFile(a);
    auto cu = std::move(builder).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(cu.crossRefs().empty());
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    std::string const text{cu.trees()[0].source().text()};
    EXPECT_NE(text.find("int b()"), std::string::npos);
    EXPECT_NE(text.find("int c()"), std::string::npos)   // transitive splice
        << "transitive quote include must be recursively inlined";
}

TEST(ImportResolver, CSubsetQuoteIncludeCycleTerminates) {
    // FC13: a circular quote-`#include` chain is broken by the preprocessor's
    // include-stack guard, which emits P_PreprocessorIncludeError on the
    // back-edge instead of looping forever. The parse still produces one tree.
    //
    // RED-ON-DISABLE: removing the `std::find(includeStack...)` cycle guard in
    // preprocessor.cpp SynthBuilder::build either hangs (infinite recursion) or
    // overflows the depth guard -- either way this test stops passing cleanly.
    TempDir dir;
    auto a = dir.write("a.h", "#include \"b.h\"\nint a() { return 0; }\n");
    dir.write("b.h", "#include \"a.h\"\nint b() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addFile(a);
    auto cu = std::move(builder).finish();

    ASSERT_EQ(cu.trees().size(), 1u);              // terminates, one tree
    // The back-edge of the cycle is reported (on the tree's reporter, remapped
    // to the originating header).
    bool sawCycleDiag = false;
    for (auto const& d : cu.trees()[0].diagnostics().all()) {
        if (d.code == DiagnosticCode::P_PreprocessorIncludeError) sawCycleDiag = true;
    }
    EXPECT_TRUE(sawCycleDiag)
        << "a circular quote-include must emit P_PreprocessorIncludeError";
}

TEST(ImportResolver, CSubsetMissingQuoteIncludeEmitsDiagnosticAndContinues) {
    // FC13: a missing quote-`#include` target is reported by the preprocessor
    // (P_PreprocessorIncludeError) and the rest of the file still parses (the
    // directive's bytes are left verbatim). Still one tree, no cross-refs.
    TempDir dir;
    auto main = dir.write("main.c", "#include \"ghost.h\"\nint main() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addFile(main);
    auto cu = std::move(builder).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(cu.crossRefs().empty());
    bool sawMissingDiag = false;
    for (auto const& d : cu.trees()[0].diagnostics().all()) {
        if (d.code == DiagnosticCode::P_PreprocessorIncludeError) sawMissingDiag = true;
    }
    EXPECT_TRUE(sawMissingDiag)
        << "a missing quote include must emit P_PreprocessorIncludeError";
    // FC13 co-existence (Fix 3): the PP OWNS the (failed) quote include and is
    // the SOLE reporter -- the post-parse import resolver must NOT also report
    // it as D_UnresolvedImport. Exactly one root cause, exactly one diagnostic
    // class (P_PreprocessorIncludeError above). RED-ON-DISABLE: reverting the
    // resolver's `if (ppEnabled) return;` quote-skip makes the resolver
    // double-report -> this count becomes 1.
    EXPECT_EQ(countCode(cu.driverDiagnostics(),
                        DiagnosticCode::D_UnresolvedImport), 0u)
        << "the resolver must not double-report a PP-owned failed quote include";
}

TEST(ImportResolver, CSubsetQuoteIncludeResolvesAcrossDirectories) {
    // FC13: the preprocessor's quote-include search honors declared include
    // dirs (addIncludeDir), mirroring the import resolver's resolveIncludePath.
    TempDir srcDir;
    TempDir incDir;
    auto main = srcDir.write("main.c", "#include \"shared.h\"\nint main() { return shared(); }\n");
    incDir.write("shared.h", "int shared() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addIncludeDir(incDir.path());
    builder.addFile(main);
    auto cu = std::move(builder).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(cu.crossRefs().empty());
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_NE(std::string{cu.trees()[0].source().text()}.find("int shared()"),
              std::string::npos)
        << "include-dir-resolved header must be spliced inline";
}

// ── FF11: angle-form `#include <h>` system-path resolution ───────────────────

// `#include <X.h>` resolves to a LANGUAGE-NEUTRAL JSON DESCRIPTOR on the SYSTEM
// search path (addSystemDir, the shippedLibDirs analogue) — NOT a c-subset
// source header (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC, the descriptor model
// that REPLACED cycle-21's source-`.h` load). The requested `<api.h>` maps to
// `api.json`; on a hit the resolver records its ABSOLUTE PATH on
// `cu.shippedLibDescriptors()` and loads NO extra Tree (a descriptor is a
// neutral symbol table, not parsed source, so it yields no CrossTreeRef). The
// semantic phase reads that path + mints the descriptor's externs (proven
// end-to-end by examples/c-subset/shipped_include_puts and by
// SemanticAnalyzerCSubset.FF11* below).
TEST(ImportResolver, CSubsetAngleIncludeResolvesToDescriptorOnSystemDir) {
    TempDir srcDir;
    TempDir sysDir;
    auto main = srcDir.write("main.c",
        "#include <api.h>\nint main() { return 0; }\n");
    // The shipped artifact is a NEUTRAL JSON descriptor, NOT a c-subset `.h`.
    auto descPath = sysDir.write("api.json",
        R"({ "library": { "pe": "lib.dll" },
             "symbols": [ { "name": "use", "signature": "fn() -> i32" } ] })");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addSystemDir(sysDir.path());       // the system (shippedLibDirs) path
    builder.addFile(main);
    auto cu = std::move(builder).finish();

    // NO second Tree: the descriptor is recorded, not loaded as source.
    ASSERT_EQ(cu.trees().size(), 1u) << "a descriptor is NOT parsed as a Tree";
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::F_ShippedHeaderNotFound));
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport));
    // A descriptor include produces NO cross-tree edge (it has no source syntax).
    EXPECT_TRUE(cu.crossRefs().empty()) << "descriptor includes yield no CrossTreeRef";

    // The resolved descriptor PATH is recorded — and it is the `api.json` we
    // wrote (stem-mapped from `<api.h>`), by weakly-canonical path identity.
    ASSERT_EQ(cu.shippedLibDescriptors().size(), 1u)
        << "the angle include must record exactly one descriptor path";
    std::error_code ec;
    auto const got  = std::filesystem::weakly_canonical(cu.shippedLibDescriptors()[0], ec);
    auto const want = std::filesystem::weakly_canonical(descPath, ec);
    EXPECT_EQ(got, want)
        << "<api.h> must map to api.json on the system dir";
}

// FF11 SUBDIRECTORY resolution: `<sys/time.h>` maps to `sys/time.json` (the
// descriptor name PRESERVES the requested subdir), DISTINCT from a top-level
// `<time.h>` -> `time.json` on the SAME systemDir. RED-ON-DISABLE: a stem-only
// mapping (`fs::path(filename).stem()`) sends `<sys/time.h>` to `time.json`,
// silently resolving the WRONG descriptor — exactly the collision this guards for
// the POSIX `sys/*` headers (SQLite-readiness Cluster G; <sys/time.h> vs <time.h>).
TEST(ImportResolver, CSubsetSubdirAngleIncludeResolvesDistinctFromTopLevel) {
    TempDir srcDir;
    TempDir sysDir;
    auto main = srcDir.write("main.c",
        "#include <sys/time.h>\nint main() { return 0; }\n");
    // A top-level DECOY with the same stem + the real subdir descriptor.
    sysDir.write("time.json",
        R"({ "header": "time.h", "typedefs": [ { "name": "DECOY_TOP", "type": "i32" } ] })");
    std::error_code mkec;
    std::filesystem::create_directories(sysDir.path() / "sys", mkec);
    auto const want = sysDir.write("sys/time.json",
        R"({ "header": "sys/time.h", "typedefs": [ { "name": "suseconds_t", "type": "i64" } ] })");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addSystemDir(sysDir.path());
    builder.addFile(main);
    auto cu = std::move(builder).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    ASSERT_EQ(cu.shippedLibDescriptors().size(), 1u)
        << "the angle include must record exactly one descriptor path";
    std::error_code ec;
    auto const got = std::filesystem::weakly_canonical(cu.shippedLibDescriptors()[0], ec);
    EXPECT_EQ(got, std::filesystem::weakly_canonical(want, ec))
        << "<sys/time.h> must resolve to sys/time.json, NOT the top-level decoy time.json";
}

// A SYSTEM-header miss is a HARD error (F_ShippedHeaderNotFound), NOT the
// soft D_UnresolvedImport the quote form uses. (FF11 fail-loud contract.)
TEST(ImportResolver, CSubsetAngleIncludeMissIsHardError) {
    TempDir srcDir;
    TempDir sysDir;   // empty — the header is absent
    auto main = srcDir.write("main.c",
        "#include <nope.h>\nint main() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addSystemDir(sysDir.path());
    builder.addFile(main);
    auto cu = std::move(builder).finish();

    EXPECT_EQ(cu.trees().size(), 1u);                // nothing loaded
    EXPECT_TRUE(cu.crossRefs().empty());
    EXPECT_EQ(countCode(cu.driverDiagnostics(),
                        DiagnosticCode::F_ShippedHeaderNotFound), 1u)
        << "a missing system header must fire F_ShippedHeaderNotFound";
    // It is NOT the soft quote-form diagnostic.
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport));
}

// The quote form does NOT search the system dir, and the angle form does
// NOT search includeDirs — the two paths are distinct (config-driven by
// pathToken vs systemPathToken, no language branch).
TEST(ImportResolver, CSubsetAngleAndQuotePathsAreDistinct) {
    TempDir srcDir;
    TempDir sysDir;
    // The header lives ONLY on the system dir. A QUOTE include of the same
    // name must NOT find it there → it fails to resolve. FC13 (Fix 3): the
    // PREPROCESSOR owns the (failed) quote include and reports
    // P_PreprocessorIncludeError; the post-parse resolver SKIPS the quote form
    // (no D_UnresolvedImport double-report). Either way it must NOT be the
    // angle-form hard error -- the two search paths stay distinct.
    auto main = srcDir.write("main.c",
        "#include \"sysonly.h\"\nint main() { return 0; }\n");
    sysDir.write("sysonly.h", "extern int s();\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addSystemDir(sysDir.path());   // declared as SYSTEM, not include
    builder.addFile(main);
    auto cu = std::move(builder).finish();

    EXPECT_EQ(cu.trees().size(), 1u);      // quote form did not reach the system dir
    // The quote-include failure is reported ONCE, by the preprocessor.
    bool sawPPInclude = false;
    for (auto const& d : cu.trees()[0].diagnostics().all()) {
        if (d.code == DiagnosticCode::P_PreprocessorIncludeError) sawPPInclude = true;
    }
    EXPECT_TRUE(sawPPInclude)
        << "a quote include not found on the (quote) search path must emit "
           "P_PreprocessorIncludeError";
    // Not double-reported by the resolver, and NOT the angle-form hard error.
    EXPECT_EQ(countCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport), 0u);
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::F_ShippedHeaderNotFound));
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
    // FC13: `#include ""` is a well-formed directive with an EMPTY filename.
    // The preprocessor resolves it to nothing and reports
    // P_PreprocessorIncludeError (it must NOT crash trying to open the
    // including directory -- the empty/`is_regular_file` guard in
    // SynthBuilder::resolveQuote). The directive bytes are left verbatim, so
    // one tree results.
    auto main = dir.write("main.c", "#include \"\"\nint main() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addFile(main);
    auto cu = std::move(builder).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    bool sawDiag = false;
    for (auto const& d : cu.trees()[0].diagnostics().all()) {
        if (d.code == DiagnosticCode::P_PreprocessorIncludeError) sawDiag = true;
    }
    EXPECT_TRUE(sawDiag)
        << "an empty quote include must be reported, never a silent skip";
}

TEST(ImportResolver, CSubsetExplicitlyAddedIncludeTargetIsNotReloaded) {
    TempDir dir;
    auto main   = dir.write("main.c", "#include \"helper.h\"\nint main() { return 0; }\n");
    auto helper = dir.write("helper.h", "int helper() { return 1; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addFile(main);
    builder.addFile(helper);   // also added explicitly
    auto cu = std::move(builder).finish();

    // FC13: main.c's quote include is INLINED by the preprocessor (no edge),
    // and the explicitly-added helper.h is its own (separately parsed) tree.
    // So there are TWO trees but ZERO cross-refs -- the include is textual.
    EXPECT_EQ(cu.trees().size(), 2u);
    EXPECT_TRUE(cu.crossRefs().empty());
    // main.c's tree carries the inlined helper text.
    EXPECT_NE(std::string{cu.trees()[0].source().text()}.find("int helper()"),
              std::string::npos);
}

TEST(ImportResolver, CSubsetSharedHeaderIsInlinedIntoEachIncluder) {
    TempDir dir;
    auto a = dir.write("a.c", "#include \"common.h\"\nint a() { return 0; }\n");
    auto b = dir.write("b.c", "#include \"common.h\"\nint b() { return 0; }\n");
    dir.write("common.h", "int common() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addFile(a);
    builder.addFile(b);
    auto cu = std::move(builder).finish();

    // FC13: each TU inlines its own copy of common.h. Two trees (a.c, b.c),
    // zero cross-refs, and BOTH trees carry the header text. (A shared header
    // is recompiled per TU -- standard C textual-include semantics.)
    EXPECT_EQ(cu.trees().size(), 2u);
    EXPECT_TRUE(cu.crossRefs().empty());
    EXPECT_NE(std::string{cu.trees()[0].source().text()}.find("int common()"),
              std::string::npos);
    EXPECT_NE(std::string{cu.trees()[1].source().text()}.find("int common()"),
              std::string::npos);
}

TEST(ImportResolver, CSubsetInMemoryIncludeResolvesViaIncludeDir) {
    TempDir incDir;
    incDir.write("dep.h", "int dep() { return 0; }\n");

    // FC13: an in-memory source's quote include is resolved by the
    // preprocessor against declared include dirs and INLINED -> one tree,
    // zero cross-refs, header text present.
    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addIncludeDir(incDir.path());
    builder.addInMemory("#include \"dep.h\"\nint main() { return dep(); }\n", "main.c");
    auto cu = std::move(builder).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(cu.crossRefs().empty());
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport));
    EXPECT_NE(std::string{cu.trees()[0].source().text()}.find("int dep()"),
              std::string::npos);
}

TEST(ImportResolver, CSubsetInMemoryIncludeWithoutIncludeDirIsUnresolved) {
    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addInMemory("#include \"dep.h\"\nint main() { return 0; }\n", "main.c");
    auto cu = std::move(builder).finish();

    EXPECT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(cu.crossRefs().empty());
    // FC13 (Fix 3): with no include dir, `dep.h` cannot be resolved. The
    // PREPROCESSOR owns the failed quote include and reports it ONCE as
    // P_PreprocessorIncludeError; the post-parse resolver SKIPS the quote form,
    // so there is NO D_UnresolvedImport double-report.
    bool sawPPInclude = false;
    for (auto const& d : cu.trees()[0].diagnostics().all()) {
        if (d.code == DiagnosticCode::P_PreprocessorIncludeError) sawPPInclude = true;
    }
    EXPECT_TRUE(sawPPInclude)
        << "an unresolvable in-memory quote include must emit "
           "P_PreprocessorIncludeError";
    EXPECT_EQ(countCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport), 0u)
        << "the resolver must not double-report a PP-owned failed quote include";
}

// FC13: a quote `#include` is a TEXTUAL splice performed by the preprocessor,
// which reads the target from DISK (not the in-memory tree registry). So an
// in-memory source registered under a path that an `#include` also names does
// NOT dedup against the include -- the preprocessor inlines the on-disk file
// text, and the in-memory source remains its own separately-parsed tree. Two
// trees, zero cross-refs; main.c's inlined text is the ON-DISK content.
// (The pre-FC13 post-parse dedup-by-label applied only to the cross-tree
// include-following arm, which is retired for C quote includes.)
TEST(ImportResolver, CSubsetInMemoryLabelDoesNotDedupAgainstTextualInclude) {
    TempDir dir;
    auto helperPath = dir.write("helper.h", "int disk_helper() { return 9; }\n");
    auto mainPath   = dir.write("main.c",
        "#include \"helper.h\"\nint main() { return 0; }\n");

    UnitBuilder builder{loadShippedSchema("c-subset")};
    builder.addInMemory("int mem_helper() { return 1; }\n", helperPath.string());
    builder.addFile(mainPath);
    auto cu = std::move(builder).finish();

    EXPECT_EQ(cu.trees().size(), 2u);
    EXPECT_TRUE(cu.crossRefs().empty());
    // main.c is tree[1] (the in-memory helper was added first as tree[0]). Its
    // inlined include text is the ON-DISK helper, proving the preprocessor
    // reads from disk for a textual splice.
    std::string const mainText{cu.trees()[1].source().text()};
    EXPECT_NE(mainText.find("disk_helper"), std::string::npos)
        << "the preprocessor splices the on-disk header text";
    EXPECT_EQ(mainText.find("mem_helper"), std::string::npos)
        << "the in-memory tree is not consulted for a textual include";
}

// ── genericity: resolution is driven by the `imports` block, not the name ─────

// Load the shipped c-subset schema TEXT, rename the language to something the
// engine has never heard of, and confirm `#include` following STILL happens.
// If any resolver code branched on the language name, the include would be
// silently dropped under the made-up name; instead the schema's `imports` block
// drives resolution, so the cross-ref appears exactly as for "CSubset".
// Load the shipped c-subset schema TEXT, rename the language to something the
// engine has never heard of, and confirm the config-SELECTED preprocessor
// STILL inlines the quote `#include`. If any pass branched on the language
// name, the include would not be inlined under the made-up name; instead the
// schema's `preprocess` block drives it, so the header text appears inline
// exactly as for "CSubset". (Pre-FC13 this asserted the post-parse
// include-following arm; FC13 retired that arm for C quote includes in favor
// of the equally config-driven preprocessor splice.)
TEST(ImportResolver, QuoteIncludeInliningIsDrivenByConfigNotLanguageName) {
    std::string text = readShippedConfigText("c-subset");
    auto const pos = text.find("\"CSubset\"");
    ASSERT_NE(pos, std::string::npos) << "shipped c-subset config no longer names CSubset";
    text.replace(pos, std::string_view{"\"CSubset\""}.size(), "\"MadeUpLang\"");

    auto loaded = GrammarSchema::loadFromText(text, "<renamed-c-subset>");
    ASSERT_TRUE(loaded.has_value())
        << "renamed schema should still load: "
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    EXPECT_EQ(schema->name(), "MadeUpLang");
    // Sanity: the config-driven preprocess block survived the rename intact.
    EXPECT_TRUE(schema->preprocess().enabled);

    TempDir dir;
    auto main = dir.write("main.c", "#include \"helper.h\"\nint main() { return helper(); }\n");
    dir.write("helper.h", "int helper() { return 1; }\n");

    UnitBuilder builder{schema};
    builder.addFile(main);
    auto cu = std::move(builder).finish();

    // The header was inlined by the preprocessor -- under a language name no
    // pass could possibly special-case. One tree, zero cross-refs, text present.
    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_TRUE(cu.crossRefs().empty());
    EXPECT_NE(std::string{cu.trees()[0].source().text()}.find("int helper()"),
              std::string::npos);
    EXPECT_EQ(countCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport), 0u);
}

// Symmetric genericity proof for the name-matching strategy: rename the
// shipped tsql-subset's `name` to an arbitrary string and confirm cross-tree
// table-name matching STILL fires. Both strategies flow through the same
// chooseResolver(*schema) -> ConfigDrivenImportResolver(schema.imports())
// path, but pinning each strategy independently leaves no room for a future
// regression that special-cased one branch on the language name.
TEST(ImportResolver, NameMatchingIsDrivenByConfigNotLanguageName) {
    std::string text = readShippedConfigText("tsql-subset");
    auto const pos = text.find("\"TsqlSubset\"");
    ASSERT_NE(pos, std::string::npos) << "shipped tsql-subset config no longer names TsqlSubset";
    text.replace(pos, std::string_view{"\"TsqlSubset\""}.size(), "\"MadeUpDb\"");

    auto loaded = GrammarSchema::loadFromText(text, "<renamed-tsql-subset>");
    ASSERT_TRUE(loaded.has_value())
        << "renamed schema should still load: "
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    EXPECT_EQ(schema->name(), "MadeUpDb");
    EXPECT_EQ(schema->imports().strategy, ImportStrategy::NameMatching);

    UnitBuilder builder{schema};
    builder.addInMemory("CREATE TABLE Users (id INT, name VARCHAR);", "schema.sql");
    builder.addInMemory("SELECT name FROM Users;", "data.sql");
    auto cu = std::move(builder).finish();

    // Resolution still fires under a language name nothing could special-case.
    ASSERT_EQ(cu.crossRefs().size(), 1u);
    EXPECT_EQ(cu.crossRefs()[0].sourceTree, cu.trees()[1].id());
    EXPECT_EQ(cu.crossRefs()[0].targetTree, cu.trees()[0].id());
    EXPECT_EQ(countCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedReference), 0u);
}
