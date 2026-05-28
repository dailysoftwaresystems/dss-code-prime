// HR11 multi-language CompilationUnit lowering: a single CU containing a
// c-subset (.c) file AND a tsql-subset (.sql) file lowers — through the SAME
// language-agnostic engine, with NO schema.name() branch — to ONE verified HIR
// module (a c `Function` decl + a `TSQL::Select` extension decl). Exercises the
// full vertical slice: CU5 per-file schema (registry + extension routing +
// explicit in-memory schema), multi-schema semantic analysis (per-tree config
// bundle), and per-schema HIR lowering into one module.
//
// DEFERRED (boundaries, not gaps; see plan 09 HR11 / plan 08 CU5): auto-discovery
// of cross-LANGUAGE reference edges (a c call resolving to a sql sproc) is FFI
// (plan 11); this file covers the uniform language-blind resolution substrate
// (same-language resolution per tree) + one-module lowering.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>
#include <string_view>

using namespace dss;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::shared_ptr<GrammarSchema const> shipped(std::string_view name) {
    auto loaded = GrammarSchema::loadShipped(name);
    if (!loaded) { ADD_FAILURE() << "loadShipped(" << name << ") failed"; std::abort(); }
    return *loaded;
}

[[nodiscard]] std::string extName(Hir const& hir, HirNodeId id) {
    if (hir.kind(id) != HirKind::Extension) return "<not-extension>";
    return std::string{hir.registry().descriptor(HirKindId{hir.payload(id)}).name()};
}

constexpr std::string_view kCProgram   = "int add(int a, int b) { return a + b; }\n";
constexpr std::string_view kSqlProgram =
    "CREATE TABLE Users (Id INT, Name VARCHAR);\n"
    "SELECT Id, Name FROM Users WHERE Id = 1;\n";

} // namespace

TEST(HirLoweringMultiLang, MixedCuLowersToOneModule) {
    auto c   = shipped("c-subset");
    auto sql = shipped("tsql-subset");

    // c-subset is the primary; tsql is registered as a second language.
    UnitBuilder ub{c};
    ub.registerSchema(sql);
    ub.addInMemory(std::string(kCProgram),   "add.c",   c);
    ub.addInMemory(std::string(kSqlProgram), "query.sql", sql);
    auto cu = std::make_shared<CompilationUnit>(std::move(ub).finish());

    ASSERT_EQ(cu->trees().size(), 2u);
    EXPECT_EQ(cu->trees()[0].schema().name(), "CSubset");
    EXPECT_EQ(cu->trees()[1].schema().name(), "TsqlSubset");

    // Multi-schema semantic analysis: each tree analyzed with its OWN config.
    SemanticModel model = analyze(cu);
    ASSERT_FALSE(model.hasErrors());

    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    // ONE module, decls in tree order: the c function, then the tsql statements
    // (CREATE TABLE + SELECT) from the .sql file.
    HirNodeId root = res->hir.root();
    ASSERT_EQ(res->hir.kind(root), HirKind::Module);
    auto decls = res->hir.moduleDecls(root);
    ASSERT_EQ(decls.size(), 3u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::Function);          // from the .c file
    EXPECT_EQ(extName(res->hir, decls[1]), "TSQL::CreateTable");    // from the .sql file
    EXPECT_EQ(extName(res->hir, decls[2]), "TSQL::Select");         // from the .sql file

    // The module's source-language label is the composite of both languages.
    std::string const lang{res->hir.sourceLanguage()};
    EXPECT_NE(lang.find("CSubset"), std::string::npos) << lang;
    EXPECT_NE(lang.find("TsqlSubset"), std::string::npos) << lang;
}

TEST(HirLoweringMultiLang, AddFileRoutesByExtension) {
    auto c   = shipped("c-subset");
    auto sql = shipped("tsql-subset");

    // Write a .c and a .sql temp file; addFile must route each to the language
    // whose declared fileExtensions match the path — no explicit schema.
    fs::path const dir = fs::temp_directory_path() / "dss_hr11_routing";
    fs::create_directories(dir);
    fs::path const cFile = dir / "unit.c", sqlFile = dir / "unit.sql";
    { std::ofstream o{cFile,   std::ios::binary}; o << kCProgram; }
    { std::ofstream o{sqlFile, std::ios::binary}; o << kSqlProgram; }

    UnitBuilder ub{c};
    ub.registerSchema(sql);
    ub.addFile(cFile);
    ub.addFile(sqlFile);
    auto cu = std::make_shared<CompilationUnit>(std::move(ub).finish());

    ASSERT_EQ(cu->trees().size(), 2u);
    EXPECT_EQ(cu->trees()[0].schema().name(), "CSubset")   << ".c must route to c-subset";
    EXPECT_EQ(cu->trees()[1].schema().name(), "TsqlSubset") << ".sql must route to tsql-subset";

    SemanticModel model = analyze(cu);
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(res->hir.moduleDecls(res->hir.root()).size(), 3u);   // c fn + sql CREATE + SELECT

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(HirLoweringMultiLang, MultiSchemaCtorAndDeclOrderFollowsTreeOrder) {
    auto c   = shipped("c-subset");
    auto sql = shipped("tsql-subset");

    // Multi-schema ctor with sql FIRST (primary); add sql then c. Decl order in
    // the one module must follow tree-add order, not schema-registration order.
    UnitBuilder ub{std::vector<std::shared_ptr<GrammarSchema const>>{sql, c}};
    ub.addInMemory(std::string(kSqlProgram), "q.sql", sql);
    ub.addInMemory(std::string(kCProgram),   "a.c",   c);
    auto cu = std::make_shared<CompilationUnit>(std::move(ub).finish());

    SemanticModel model = analyze(cu);
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 3u);
    EXPECT_EQ(extName(res->hir, decls[0]), "TSQL::CreateTable"); // sql tree added first
    EXPECT_EQ(extName(res->hir, decls[1]), "TSQL::Select");      // sql tree added first
    EXPECT_EQ(res->hir.kind(decls[2]), HirKind::Function);       // c tree added second
}

TEST(HirLoweringMultiLang, UnknownExtensionIsLoudInMultiLanguageCu) {
    // In a multi-language CU, an addFile whose extension matches NO registered
    // language fails loud (D_UnknownFileExtension) and adds no tree — never
    // silently parses under the primary grammar.
    auto c   = shipped("c-subset");
    auto sql = shipped("tsql-subset");
    fs::path const dir = fs::temp_directory_path() / "dss_hr11_unknown_ext";
    fs::create_directories(dir);
    fs::path const py = dir / "script.py";
    { std::ofstream o{py, std::ios::binary}; o << "print(1)\n"; }

    UnitBuilder ub{c};
    ub.registerSchema(sql);
    ub.addFile(py);
    auto cu = std::make_shared<CompilationUnit>(std::move(ub).finish());

    EXPECT_TRUE(cu->trees().empty()) << "an unroutable file must not be parsed";
    std::size_t unknownExt = 0;
    for (auto const& d : cu->driverDiagnostics().all())
        if (d.code == DiagnosticCode::D_UnknownFileExtension) ++unknownExt;
    EXPECT_EQ(unknownExt, 1u) << "an unroutable extension must be loud";

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(HirLoweringMultiLang, ExplicitInMemorySchemaNeedNotBePreRegistered) {
    // addInMemory with an explicit schema that was NEVER registerSchema'd: the
    // schema is auto-registered at parse, so the tree still gets its own
    // language's semantic analysis + import resolution + lowering (no silent
    // skip). Proves the resolver loop covers every tree's schema, not just the
    // pre-registered set.
    auto c   = shipped("c-subset");
    auto sql = shipped("tsql-subset");
    UnitBuilder ub{c};                                    // only c registered
    ub.addInMemory(std::string(kCProgram), "a.c", c);
    ub.addInMemory(std::string(kSqlProgram), "q.sql", sql);  // sql NOT pre-registered
    auto cu = std::make_shared<CompilationUnit>(std::move(ub).finish());

    ASSERT_EQ(cu->trees().size(), 2u);
    SemanticModel model = analyze(cu);
    ASSERT_FALSE(model.hasErrors())
        << "the un-pre-registered sql tree must analyze under its own schema";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(res->hir.moduleDecls(res->hir.root()).size(), 3u);
}

TEST(HirLoweringMultiLang, BothLanguagesBuiltinsAndExtensionsCoexist) {
    // A tsql tree using COALESCE (a tsql builtin) AND a c tree must both resolve:
    // each tree sees its own language's builtins, and tsql's TSQL::* extension
    // kinds coexist with c's core kinds in the one HIR kind registry —
    // verify-on-load (res->ok) is the proof.
    auto c   = shipped("c-subset");
    auto sql = shipped("tsql-subset");
    UnitBuilder ub{c};
    ub.registerSchema(sql);
    ub.addInMemory("int f(int x) { return x; }\n", "f.c", c);
    ub.addInMemory("CREATE TABLE T (a INT);\n"
                   "SELECT COALESCE(a, 0) FROM T;\n", "q.sql", sql);
    auto cu = std::make_shared<CompilationUnit>(std::move(ub).finish());

    SemanticModel model = analyze(cu);
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

namespace {
[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == code) ++n;
    return n;
}
}

TEST(HirLoweringMultiLang, SameLanguageCrossTreeResolvesAroundInterleavedOtherLang) {
    // Two tsql files (one defines Users, one references it) + an interleaved c
    // file in ONE CU. The tsql name-matching resolver must resolve the reference
    // ACROSS its two trees (producing exactly one cross-ref edge B→A) while
    // correctly SKIPPING the c tree (owns() gating) — and the c include-following
    // resolver must produce no spurious edge.
    auto c   = shipped("c-subset");
    auto sql = shipped("tsql-subset");
    UnitBuilder ub{c};
    ub.registerSchema(sql);
    ub.addInMemory("CREATE TABLE Users (Id INT, Name VARCHAR);\n", "users.sql", sql); // tree 0 (def)
    ub.addInMemory("int add(int a, int b) { return a + b; }\n",    "add.c",     c);   // tree 1 (c)
    ub.addInMemory("SELECT Id FROM Users WHERE Id = 1;\n",          "query.sql", sql); // tree 2 (ref)
    auto cu = std::make_shared<CompilationUnit>(std::move(ub).finish());

    ASSERT_EQ(cu->trees().size(), 3u);
    // Exactly one cross-ref: the query.sql reference to users.sql's Users table.
    auto xrefs = cu->crossRefs();
    ASSERT_EQ(xrefs.size(), 1u) << "tsql name-match must resolve across its two trees, once";
    EXPECT_EQ(xrefs[0].sourceTree.v, cu->trees()[2].id().v);   // query.sql references
    EXPECT_EQ(xrefs[0].targetTree.v, cu->trees()[0].id().v);   // users.sql defines
    // No unresolved-reference diagnostic (the c tree was skipped, not mis-scanned).
    EXPECT_EQ(countCode(cu->driverDiagnostics(), DiagnosticCode::D_UnresolvedReference), 0u);

    SemanticModel model = analyze(cu);
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

TEST(HirLoweringMultiLang, DuplicateRegisterSchemaDoesNotDoubleResolve) {
    // registerSchema does not dedup, but finish() runs ONE resolver per distinct
    // SchemaId — so registering tsql twice must NOT double-append the cross-ref.
    auto c   = shipped("c-subset");
    auto sql = shipped("tsql-subset");
    UnitBuilder ub{c};
    ub.registerSchema(sql);
    ub.registerSchema(sql);   // duplicate — must not cause double resolution
    ub.addInMemory("CREATE TABLE Users (Id INT);\n",      "users.sql", sql);
    ub.addInMemory("SELECT Id FROM Users WHERE Id = 1;\n", "query.sql", sql);
    auto cu = std::make_shared<CompilationUnit>(std::move(ub).finish());

    EXPECT_EQ(cu->crossRefs().size(), 1u) << "duplicate registration must not double-append edges";
}

TEST(HirLoweringMultiLang, BuiltinsDoNotLeakAcrossLanguages) {
    // COALESCE is a tsql builtin; c-subset declares none. In a mixed CU a c file
    // calling COALESCE must NOT silently resolve it (that would be a cross-language
    // reference, which is the FFI plan's job — not a builtin hit). Per-language
    // builtin scopes enforce this: the c tree only sees c's (empty) builtins.
    auto c   = shipped("c-subset");
    auto sql = shipped("tsql-subset");
    UnitBuilder ub{c};
    ub.registerSchema(sql);
    ub.addInMemory("int f(int x) { return COALESCE(x, 0); }\n", "f.c", c);   // c calls a tsql builtin
    ub.addInMemory("CREATE TABLE T (a INT);\n", "t.sql", sql);
    auto cu = std::make_shared<CompilationUnit>(std::move(ub).finish());

    SemanticModel model = analyze(cu);
    EXPECT_GT(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "a c-subset tree must NOT resolve tsql's COALESCE builtin";
}
