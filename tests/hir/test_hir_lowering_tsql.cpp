// HR10 CST→HIR lowering tests for the shipped `tsql-subset` SQL language: each
// SQL statement lowers to a role-explicit extension node (TSQL::Select / Insert
// / Update / Delete / CreateTable, with TSQL::Projection / ValueList / AssignList
// / Columns groupings), conditions reuse core BinaryOp/Literal/Ref, string VALUES
// coalesce + decode to Array<Char,N> literals, and SQL `NULL` lowers to a leaf
// TSQL::Null extension operand. The single generic engine drives all of this from
// config alone — there is NO SQL vocabulary in src/hir/lowering, which this file
// exercises end-to-end through the shipped grammar.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "hir/hir.hpp"
#include "hir/hir_text.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace dss;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == c) ++n;
    return n;
}

// Drive: tsql-subset source → CompilationUnit → SemanticModel. Asserts the front
// end (parse + semantic) is clean so a lowering test never chases a phantom.
[[nodiscard]] SemanticModel analyzeTsql(std::string src) {
    auto loaded = GrammarSchema::loadShipped("tsql-subset");
    if (!loaded) { ADD_FAILURE() << "loadShipped(tsql-subset) failed"; std::abort(); }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    return analyze(cu);
}

[[nodiscard]] std::vector<std::string> symbolNames(SemanticModel const& m) {
    std::vector<std::string> names;
    auto const& syms = m.symbols();
    names.reserve(syms.size());
    for (auto const& s : syms) names.push_back(s.name);
    return names;
}

// The registered extension-kind name of an Extension node (e.g. "TSQL::Select").
[[nodiscard]] std::string extName(Hir const& hir, HirNodeId id) {
    if (hir.kind(id) != HirKind::Extension) return "<not-extension>";
    return std::string{hir.registry().descriptor(HirKindId{hir.payload(id)}).name()};
}

// A schema (just CREATE TABLE) so DML below has tables to resolve `FROM`/INTO
// against — tsql resolves TABLE refs at module scope.
constexpr std::string_view kSchema =
    "CREATE TABLE Users (Id INT, Name VARCHAR, Active BIT);\n"
    "CREATE TABLE Orders (Id INT, UserId INT, Amount INT, Status VARCHAR);\n";

} // namespace

TEST(HirLoweringTsql, CreateTableLowersToColumnsOfVarDecls) {
    SemanticModel model = analyzeTsql(
        "CREATE TABLE Users (Id INT, Name VARCHAR, Active BIT);\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(extName(res->hir, decls[0]), "TSQL::CreateTable");
    // children: [ name (TSQL::Name), TSQL::Columns ]
    auto kids = res->hir.children(decls[0]);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(extName(res->hir, kids[0]), "TSQL::Name");          // table name
    EXPECT_EQ(extName(res->hir, kids[1]), "TSQL::Columns");
    // Each column is a core VarDecl (role-explicit reuse of the core kind).
    auto cols = res->hir.children(kids[1]);
    ASSERT_EQ(cols.size(), 3u);
    for (HirNodeId c : cols) EXPECT_EQ(res->hir.kind(c), HirKind::VarDecl);
}

TEST(HirLoweringTsql, SelectStarLowersToStarProjection) {
    SemanticModel model = analyzeTsql(std::string(kSchema) + "SELECT * FROM Users;\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 3u);                                 // 2 CREATE + 1 SELECT
    HirNodeId sel = decls[2];
    EXPECT_EQ(extName(res->hir, sel), "TSQL::Select");
    // children: [ projection (TSQL::Star), from (TSQL::Name) ] — no WHERE.
    auto kids = res->hir.children(sel);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(extName(res->hir, kids[0]), "TSQL::Star");
    EXPECT_EQ(extName(res->hir, kids[1]), "TSQL::Name");         // FROM Users
}

TEST(HirLoweringTsql, SelectProjectionAndWhereReuseCoreNodes) {
    SemanticModel model = analyzeTsql(
        std::string(kSchema) + "SELECT Id, Name FROM Users WHERE Active = 1;\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId sel = res->hir.moduleDecls(res->hir.root())[2];
    EXPECT_EQ(extName(res->hir, sel), "TSQL::Select");
    auto kids = res->hir.children(sel);
    ASSERT_EQ(kids.size(), 3u);                                  // projection, from, where
    // Projection is a TSQL::Projection of two column Refs (core reuse).
    EXPECT_EQ(extName(res->hir, kids[0]), "TSQL::Projection");
    auto items = res->hir.children(kids[0]);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(extName(res->hir, items[0]), "TSQL::Name");        // Id
    EXPECT_EQ(extName(res->hir, items[1]), "TSQL::Name");        // Name
    // WHERE condition is a plain core BinaryOp (Active = 1): a relational-name
    // operand and a typed literal. The comparison itself is typed (Bool).
    EXPECT_EQ(res->hir.kind(kids[2]), HirKind::BinaryOp);
    auto cmp = res->hir.children(kids[2]);
    ASSERT_EQ(cmp.size(), 2u);
    EXPECT_EQ(extName(res->hir, cmp[0]), "TSQL::Name");          // Active
    EXPECT_EQ(res->hir.kind(cmp[1]), HirKind::Literal);          // 1
}

TEST(HirLoweringTsql, InsertStringValueLowersToCharArrayLiteral) {
    SemanticModel model = analyzeTsql(
        std::string(kSchema) +
        "INSERT INTO Users VALUES (1, 'Alice', 1);\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId ins = res->hir.moduleDecls(res->hir.root())[2];
    EXPECT_EQ(extName(res->hir, ins), "TSQL::Insert");
    // children: [ target (TSQL::Name), values (TSQL::ValueList) ] — no column list.
    auto kids = res->hir.children(ins);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(extName(res->hir, kids[0]), "TSQL::Name");        // INTO Users
    EXPECT_EQ(extName(res->hir, kids[1]), "TSQL::ValueList");
    auto vals = res->hir.children(kids[1]);
    ASSERT_EQ(vals.size(), 3u);
    EXPECT_EQ(res->hir.kind(vals[0]), HirKind::Literal);        // 1
    EXPECT_EQ(res->hir.kind(vals[1]), HirKind::Literal);        // 'Alice'
    EXPECT_EQ(res->hir.kind(vals[2]), HirKind::Literal);        // 1

    // The string VALUE decoded to its bytes, typed Array<Char, 6> ('Alice' + NUL).
    auto const& ti = model.lattice().interner();
    TypeId const sty = res->hir.typeId(vals[1]);
    ASSERT_EQ(ti.kind(sty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(sty)[0], 6);                           // 5 chars + NUL
    EXPECT_EQ(ti.kind(ti.operands(sty)[0]), TypeKind::Char);
    bool foundAlice = false;
    for (std::size_t i = 0; i < res->literalPool.size(); ++i) {
        auto const& v = res->literalPool.at(i);
        if (std::holds_alternative<std::string>(v.value)
            && std::get<std::string>(v.value) == "Alice") foundAlice = true;
    }
    EXPECT_TRUE(foundAlice) << "the decoded 'Alice' bytes must be in the literal pool";
}

TEST(HirLoweringTsql, DoubledDelimiterStringValueDecodes) {
    // `'Bob''s'` is the literal `Bob's` — the coalesced body keeps `''` raw and
    // the lowering's doubled-delimiter decoder collapses it.
    SemanticModel model = analyzeTsql(
        std::string(kSchema) +
        "INSERT INTO Users VALUES (2, 'Bob''s', 1);\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    bool foundBobs = false;
    for (std::size_t i = 0; i < res->literalPool.size(); ++i) {
        auto const& v = res->literalPool.at(i);
        if (std::holds_alternative<std::string>(v.value)
            && std::get<std::string>(v.value) == "Bob's") foundBobs = true;
    }
    EXPECT_TRUE(foundBobs) << "doubled-delim `''` must decode to a single `'`";
}

TEST(HirLoweringTsql, UnicodeStringValueLowersToCharArray) {
    // `N'héllo'` — the unicode-string opener shares the coalesced body token and
    // the same decoder, so the VALUE lowers to an Array<Char,N> just like a plain
    // string. Multi-byte UTF-8 (é = 0xC3 0xA9) passes through verbatim, so the
    // decoded bytes are 6 and the type is Array<Char, 7> (6 bytes + NUL).
    SemanticModel model = analyzeTsql(
        std::string(kSchema) +
        "INSERT INTO Users VALUES (1, N'h\xC3\xA9llo', 1);\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId ins  = res->hir.moduleDecls(res->hir.root())[2];
    HirNodeId vals = res->hir.children(ins)[1];                 // TSQL::ValueList
    HirNodeId sv   = res->hir.children(vals)[1];                // the N'…' value
    ASSERT_EQ(res->hir.kind(sv), HirKind::Literal);
    auto const& ti = model.lattice().interner();
    TypeId const sty = res->hir.typeId(sv);
    ASSERT_EQ(ti.kind(sty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(sty)[0], 7);                           // 6 raw bytes + NUL
    EXPECT_EQ(ti.kind(ti.operands(sty)[0]), TypeKind::Char);
    bool found = false;
    for (std::size_t i = 0; i < res->literalPool.size(); ++i) {
        auto const& v = res->literalPool.at(i);
        if (std::holds_alternative<std::string>(v.value)
            && std::get<std::string>(v.value) == "h\xC3\xA9llo") found = true;
    }
    EXPECT_TRUE(found) << "the decoded multi-byte N'…' bytes must be in the pool";
}

TEST(HirLoweringTsql, NullValueLowersToNullExtensionOperand) {
    SemanticModel model = analyzeTsql(
        std::string(kSchema) +
        "INSERT INTO Users VALUES (3, NULL, 0);\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId ins  = res->hir.moduleDecls(res->hir.root())[2];
    HirNodeId vals = res->hir.children(ins)[1];                 // TSQL::ValueList
    auto items = res->hir.children(vals);
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(res->hir.kind(items[0]), HirKind::Literal);       // 3
    EXPECT_EQ(extName(res->hir, items[1]), "TSQL::Null");       // NULL → extension leaf
    EXPECT_TRUE(res->hir.children(items[1]).empty()) << "TSQL::Null is a leaf";
    EXPECT_EQ(res->hir.kind(items[2]), HirKind::Literal);       // 0
}

TEST(HirLoweringTsql, FunctionCallInExpressionLowersToCall) {
    // A SQL function call (`COALESCE(Amount, 0)`) inside an expression lowers to a
    // core Call: callee Ref + lowered argument expressions. This pins that the
    // flat-operand path detects the call rule (it nests under `nameOrCall →
    // callExpr`, which `peelToCore` would otherwise skip past) rather than
    // collapsing the whole thing to a bare name.
    SemanticModel model = analyzeTsql(
        std::string(kSchema) +
        "SELECT Id FROM Orders WHERE Amount = COALESCE(Amount, 0);\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId sel  = res->hir.moduleDecls(res->hir.root())[2];
    HirNodeId where = res->hir.children(sel)[2];                // BinaryOp: Amount = COALESCE(...)
    ASSERT_EQ(res->hir.kind(where), HirKind::BinaryOp);
    HirNodeId rhs = res->hir.children(where)[1];
    ASSERT_EQ(res->hir.kind(rhs), HirKind::Call) << "the call must not collapse to a bare name";
    // [callee (TSQL::Name — function names are relational), arg Amount, arg 0].
    auto callKids = res->hir.children(rhs);
    ASSERT_EQ(callKids.size(), 3u);
    EXPECT_EQ(extName(res->hir, callKids[0]), "TSQL::Name");    // COALESCE callee
    EXPECT_EQ(extName(res->hir, callKids[1]), "TSQL::Name");    // Amount
    EXPECT_EQ(res->hir.kind(callKids[2]), HirKind::Literal);    // 0
}

TEST(HirLoweringTsql, InsertWithColumnListLowers) {
    SemanticModel model = analyzeTsql(
        std::string(kSchema) +
        "INSERT INTO Users (Id, Name) VALUES (1, 'A');\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId ins = res->hir.moduleDecls(res->hir.root())[2];
    auto kids = res->hir.children(ins);
    ASSERT_EQ(kids.size(), 3u);                                 // target, columns, values
    EXPECT_EQ(extName(res->hir, kids[0]), "TSQL::Name");
    EXPECT_EQ(extName(res->hir, kids[1]), "TSQL::ColumnList");
    auto cols = res->hir.children(kids[1]);
    ASSERT_EQ(cols.size(), 2u);                                 // Id, Name
    EXPECT_EQ(extName(res->hir, cols[0]), "TSQL::Name");
    EXPECT_EQ(extName(res->hir, cols[1]), "TSQL::Name");
    EXPECT_EQ(extName(res->hir, kids[2]), "TSQL::ValueList");
}

TEST(HirLoweringTsql, UpdateLowersToAssignList) {
    SemanticModel model = analyzeTsql(
        std::string(kSchema) +
        "UPDATE Orders SET Amount = Amount + 100 WHERE Id = 1;\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId upd = res->hir.moduleDecls(res->hir.root())[2];
    EXPECT_EQ(extName(res->hir, upd), "TSQL::Update");
    auto kids = res->hir.children(upd);
    ASSERT_EQ(kids.size(), 3u);                                 // target, assignments, where
    EXPECT_EQ(extName(res->hir, kids[0]), "TSQL::Name");        // Orders
    EXPECT_EQ(extName(res->hir, kids[1]), "TSQL::AssignList");
    auto assigns = res->hir.children(kids[1]);
    ASSERT_EQ(assigns.size(), 1u);
    EXPECT_EQ(extName(res->hir, assigns[0]), "TSQL::Assignment");
    // Assignment: [ target (TSQL::Name), value ] where value is `Amount + 100`.
    auto a = res->hir.children(assigns[0]);
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(extName(res->hir, a[0]), "TSQL::Name");
    EXPECT_EQ(res->hir.kind(a[1]), HirKind::BinaryOp);
    EXPECT_EQ(res->hir.kind(kids[2]), HirKind::BinaryOp);       // WHERE Id = 1
}

TEST(HirLoweringTsql, DeleteWithWhereLowers) {
    SemanticModel model = analyzeTsql(
        std::string(kSchema) +
        "DELETE FROM Orders WHERE Status = 'cancelled';\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId del = res->hir.moduleDecls(res->hir.root())[2];
    EXPECT_EQ(extName(res->hir, del), "TSQL::Delete");
    auto kids = res->hir.children(del);
    ASSERT_EQ(kids.size(), 2u);                                 // target, where
    EXPECT_EQ(extName(res->hir, kids[0]), "TSQL::Name");        // FROM Orders
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::BinaryOp);       // Status = 'cancelled'
}

// ── golden ────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] fs::path findLoweringGoldens() {
    fs::path cwd = fs::current_path();
    for (int hops = 0; hops < 8; ++hops) {
        auto const cand = cwd / "tests" / "hir" / "lowering_goldens";
        if (fs::is_directory(cand)) return cand;
        if (!cwd.has_parent_path() || cwd == cwd.parent_path()) break;
        cwd = cwd.parent_path();
    }
    ADD_FAILURE() << "could not locate tests/hir/lowering_goldens from "
                  << fs::current_path().string();
    std::abort();
}

[[nodiscard]] bool goldenRefreshRequested() {
    char const* raw = std::getenv("DSS_REFRESH_GOLDENS");
    if (raw == nullptr) return false;
    std::string_view const v{raw};
    return v == "1" || v == "true" || v == "TRUE" || v == "yes";
}

[[nodiscard]] std::string readFile(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) { ADD_FAILURE() << "cannot open " << p.string(); std::abort(); }
    std::ostringstream buf; buf << in.rdbuf();
    std::string s = std::move(buf).str();
    std::erase(s, '\r');   // CRLF→LF: golden compare is line-ending agnostic (Windows autocrlf)
    return s;
}

} // namespace

TEST(HirLoweringTsql, GoldenRepresentativeProgram) {
    SemanticModel model = analyzeTsql(
        "CREATE TABLE Users (Id INT, Name VARCHAR, Active BIT);\n"
        "INSERT INTO Users (Id, Name, Active) VALUES (1, 'Alice', 1);\n"
        "INSERT INTO Users VALUES (2, NULL, 0);\n"
        "UPDATE Users SET Active = 0 WHERE Id = 1;\n"
        "SELECT Id, Name FROM Users WHERE Active = 1;\n"
        "DELETE FROM Users WHERE Id = 2;\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_GT(res->sourceMap.size(), 0u) << "lowering did not populate source provenance";

    std::vector<std::string> names = symbolNames(model);
    HirTextContext ctx;
    ctx.interner    = &model.lattice().interner();
    ctx.symbolNames = &names;
    ctx.literalPool = &res->literalPool;
    DiagnosticReporter er;
    std::string const out = emitHir(res->hir, ctx, er);

    fs::path golden = findLoweringGoldens() / "tsql_dml.dsshir";
    if (goldenRefreshRequested()) {
        std::ofstream o{golden, std::ios::binary}; o << out;
        ADD_FAILURE() << "Refreshed " << golden.string()
                      << " — refresh is developer-only; the test fails by design.";
        return;
    }
    if (!fs::exists(golden)) {
        ADD_FAILURE() << "missing golden " << golden.string()
                      << " — generate via DSS_REFRESH_GOLDENS=1";
        return;
    }
    EXPECT_EQ(out, readFile(golden));

    // The emitted lowering (extension preamble + ext_node forms) must itself be a
    // valid, parseable, verifiable .dsshir module.
    DiagnosticReporter pr;
    auto parsed = parseHir(out, CompilationUnitId{1}, pr);
    std::string diags;
    for (auto const& d : pr.all()) diags += std::string{diagnosticCodeName(d.code)} + ": " + d.actual + "\n";
    EXPECT_TRUE(parsed->ok) << "lowered .dsshir did not round-trip/verify\n" << diags;
}
