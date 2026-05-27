// HR8 CST→HIR lowering tests: end-to-end (parse c-subset → semantic → lowerToHir
// → verify) over the covered c-subset slice, a deferred-construct diagnostic, and
// a `.dsshir` golden of a representative program. Genericity (no schema.name()
// dependence) is guaranteed by construction — the engine never inspects the
// language name — and demonstrated here by lowering a real shipped language
// through the single generic engine.

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
#include <vector>

using namespace dss;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == c) ++n;
    return n;
}

// Drive: c-subset source → CompilationUnit → SemanticModel. Asserts the front
// end (parse + semantic) is clean so a lowering test never chases a phantom.
[[nodiscard]] SemanticModel analyzeCSubset(std::string src) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) { ADD_FAILURE() << "loadShipped(c-subset) failed"; std::abort(); }
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

} // namespace

TEST(HirLoweringCSubset, EmptyVoidFunction) {
    SemanticModel model = analyzeCSubset("void f() {}");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok);
    HirNodeId root = res->hir.root();
    ASSERT_EQ(res->hir.kind(root), HirKind::Module);
    auto decls = res->hir.moduleDecls(root);
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::Function);
    // Function body is a Block.
    EXPECT_EQ(res->hir.kind(res->hir.functionBody(decls[0])), HirKind::Block);
}

TEST(HirLoweringCSubset, ReturnLiteralPopulatesPool) {
    SemanticModel model = analyzeCSubset("int f() { return 42; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(v.value));
    EXPECT_EQ(std::get<std::int64_t>(v.value), 42);
    EXPECT_EQ(v.core, TypeKind::I32);
}

TEST(HirLoweringCSubset, ArithmeticAndParams) {
    SemanticModel model = analyzeCSubset("int add(int a, int b) { return a + b; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = res->hir.moduleDecls(res->hir.root())[0];
    ASSERT_EQ(res->hir.kind(fn), HirKind::Function);
    EXPECT_EQ(res->hir.functionParams(fn).size(), 2u);          // a, b
    HirNodeId body = res->hir.functionBody(fn);
    auto stmts = res->hir.children(body);
    ASSERT_EQ(stmts.size(), 1u);
    ASSERT_EQ(res->hir.kind(stmts[0]), HirKind::ReturnStmt);
    HirNodeId ret = *res->hir.returnValue(stmts[0]);
    EXPECT_EQ(res->hir.kind(ret), HirKind::BinaryOp);           // a + b
}

TEST(HirLoweringCSubset, ControlFlowAndAssignment) {
    SemanticModel model = analyzeCSubset(
        "void f(int x) {\n"
        "  while (x) {\n"
        "    if (x) { x = x + 1; } else { x = 0; }\n"
        "  }\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = res->hir.moduleDecls(res->hir.root())[0];
    HirNodeId body = res->hir.functionBody(fn);
    auto stmts = res->hir.children(body);
    ASSERT_EQ(stmts.size(), 1u);
    EXPECT_EQ(res->hir.kind(stmts[0]), HirKind::WhileStmt);
}

TEST(HirLoweringCSubset, ForLoop) {
    SemanticModel model = analyzeCSubset(
        "void f() { for (int i = 0; i < 10; i = i + 1) {} }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = res->hir.moduleDecls(res->hir.root())[0];
    HirNodeId body = res->hir.functionBody(fn);
    HirNodeId forS = res->hir.children(body)[0];
    ASSERT_EQ(res->hir.kind(forS), HirKind::ForStmt);
    EXPECT_TRUE(res->hir.forInit(forS).has_value());
    EXPECT_TRUE(res->hir.loopCondition(forS).has_value());
    EXPECT_TRUE(res->hir.forUpdate(forS).has_value());
}

TEST(HirLoweringCSubset, SwitchGroupsFlatCases) {
    SemanticModel model = analyzeCSubset(
        "void f(int x) { switch (x) { case 1: break; default: break; } }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = res->hir.moduleDecls(res->hir.root())[0];
    HirNodeId sw = res->hir.children(res->hir.functionBody(fn))[0];
    ASSERT_EQ(res->hir.kind(sw), HirKind::SwitchStmt);
    auto arms = res->hir.switchArms(sw);
    ASSERT_EQ(arms.size(), 2u);
    EXPECT_FALSE(res->hir.caseArmIsDefault(arms[0]));   // case 1
    EXPECT_TRUE(res->hir.caseArmIsDefault(arms[1]));     // default
}

TEST(HirLoweringCSubset, CallAndTypedef) {
    SemanticModel model = analyzeCSubset(
        "typedef int myint;\n"
        "int g(int a) { return a; }\n"
        "int f(int x) { return g(x); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 3u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::TypeDecl);     // typedef
    EXPECT_EQ(res->hir.kind(decls[1]), HirKind::Function);     // g
    EXPECT_EQ(res->hir.kind(decls[2]), HirKind::Function);     // f
    // f's body returns a Call.
    HirNodeId fbody = res->hir.functionBody(decls[2]);
    HirNodeId ret = res->hir.children(fbody)[0];
    EXPECT_EQ(res->hir.kind(*res->hir.returnValue(ret)), HirKind::Call);
}

TEST(HirLoweringCSubset, ExternFunctionAndGlobal) {
    SemanticModel model = analyzeCSubset(
        "extern int puts(int c);\n"
        "extern int errcode;\n"
        "int f() { return puts(0); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 3u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::ExternFunction);   // puts
    EXPECT_EQ(res->hir.externFunctionParams(decls[0]).size(), 1u);  // c
    EXPECT_EQ(res->hir.kind(decls[1]), HirKind::ExternGlobal);     // errcode
    EXPECT_EQ(res->hir.kind(decls[2]), HirKind::Function);         // f
}

TEST(HirLoweringCSubset, GlobalVariable) {
    SemanticModel model = analyzeCSubset("int counter;\nint f() { return 0; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 2u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::Global);
}

TEST(HirLoweringCSubset, CompoundAssignLowers) {
    // `x += 1` → `x = x + 1`: a simple variable lvalue is read twice safely.
    SemanticModel model = analyzeCSubset("void f(int x) { x += 1; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId stmt = res->hir.children(body)[0];
    ASSERT_EQ(res->hir.kind(stmt), HirKind::AssignStmt);
    EXPECT_EQ(res->hir.kind(res->hir.assignValue(stmt)), HirKind::BinaryOp);  // x + 1
}

TEST(HirLoweringCSubset, IncrementInStatementPositionLowers) {
    // `x++;` (value discarded) → `x = x + 1`.
    SemanticModel model = analyzeCSubset("void f(int x) { x++; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    EXPECT_EQ(res->hir.kind(res->hir.children(body)[0]), HirKind::AssignStmt);
}

TEST(HirLoweringCSubset, ForUpdateIncrement) {
    SemanticModel model = analyzeCSubset("void f() { for (int i = 0; i < 3; i++) {} }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId forS = res->hir.children(res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]))[0];
    ASSERT_EQ(res->hir.kind(forS), HirKind::ForStmt);
    EXPECT_TRUE(res->hir.forUpdate(forS).has_value());
}

TEST(HirLoweringCSubset, ValueYieldingIncrementIsDeferred) {
    // `y = x++` needs sequencing (yield old value then mutate) HIR lacks.
    SemanticModel model = analyzeCSubset("int f(int x) { return x++; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GT(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 0u);
}

TEST(HirLoweringCSubset, ArrayDeclarationLowersToArrayType) {
    // `int a[10]` lowers to a local VarDecl whose type is Array<I32, 10>. (HR9
    // un-deferred arrays: the semantic phase folds the `[10]` declarator suffix
    // into the element type via a constant-length eval.)
    SemanticModel model = analyzeCSubset("void f() { int a[10]; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId vd   = res->hir.children(body)[0];
    ASSERT_EQ(res->hir.kind(vd), HirKind::VarDecl);
    TypeId const ty = res->hir.varDeclType(vd);
    auto const& ti  = model.lattice().interner();
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    ASSERT_EQ(ti.scalars(ty).size(), 1u);
    EXPECT_EQ(ti.scalars(ty)[0], 10);                          // length
    ASSERT_EQ(ti.operands(ty).size(), 1u);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::I32);     // element
}

TEST(HirLoweringCSubset, NonConstantArrayLengthFailsLoud) {
    // `int a[n]` (variable length) and `int a[]` (no length) must NOT silently
    // decay or assume a length — the semantic phase emits
    // S_NonConstantArrayLength and the symbol type stays unresolved.
    SemanticModel model = analyzeCSubset("void f(int n) { int a[n]; }");
    EXPECT_TRUE(model.hasErrors());
}

TEST(HirLoweringCSubset, IntegerOverflowReported) {
    // A literal too large for the 64-bit decoder must be reported, not silently
    // wrapped modulo 2^64.
    SemanticModel model = analyzeCSubset("int f() { return 99999999999999999999999; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GT(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 0u);
}

TEST(HirLoweringCSubset, StringLiteralIsDeferred) {
    // c-subset has no string type; a string-literal operand must fail loud.
    SemanticModel model = analyzeCSubset("int f() { return \"x\"; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GT(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 0u);
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
    std::ostringstream buf; buf << in.rdbuf(); return std::move(buf).str();
}

} // namespace

TEST(HirLoweringCSubset, GoldenRepresentativeProgram) {
    SemanticModel model = analyzeCSubset(
        "int add(int a, int b) {\n"
        "  return a + b;\n"
        "}\n"
        "int main() {\n"
        "  int x = add(1, 2);\n"
        "  if (x) {\n"
        "    x = x + 1;\n"
        "  }\n"
        "  return x;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    // Source provenance IS populated (asserted below), but the golden omits it:
    // BufferId is a process-global monotonic id, so `@loc(buf N, …)` would depend
    // on how many buffers earlier tests minted. The structure + types + symbols
    // are fully deterministic.
    EXPECT_GT(res->sourceMap.size(), 0u) << "lowering did not populate source provenance";

    std::vector<std::string> names = symbolNames(model);
    HirTextContext ctx;
    ctx.interner    = &model.lattice().interner();
    ctx.symbolNames = &names;
    DiagnosticReporter er;
    std::string const out = emitHir(res->hir, ctx, er);

    fs::path golden = findLoweringGoldens() / "c_subset_add_main.dsshir";
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

    // The emitted lowering must itself be a valid, parseable .dsshir module.
    DiagnosticReporter pr;
    auto parsed = parseHir(out, CompilationUnitId{1}, pr);
    std::string diags;
    for (auto const& d : pr.all()) diags += std::string{diagnosticCodeName(d.code)} + ": " + d.actual + "\n";
    EXPECT_TRUE(parsed->ok) << "lowered .dsshir did not round-trip/verify\n" << diags;
}
