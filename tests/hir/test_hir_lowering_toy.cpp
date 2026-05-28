// HR9 CST→HIR lowering tests for the enriched `toy` language. The point of
// this file is genericity-by-demonstration: the SAME generic engine that
// lowers c-subset (test_hir_lowering_toy's sibling) lowers toy with zero
// toy-specific C++ — only `toy.lang.json`'s `hirLowering` block differs.
// Toy is a smaller, assignment-free language (only `var` binds), so it
// exercises the engine on a deliberately different grammar shape:
// `func name(params) -> type { ... }`, value-less `return`, and a Pratt
// expression whose atom is `operand`.

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

// Drive: toy source → CompilationUnit → SemanticModel. Asserts the front end
// (parse + semantic) is clean so a lowering test never chases a phantom.
[[nodiscard]] SemanticModel analyzeToy(std::string src) {
    auto loaded = GrammarSchema::loadShipped("toy");
    if (!loaded) { ADD_FAILURE() << "loadShipped(toy) failed"; std::abort(); }
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

TEST(HirLoweringToy, EmptyVoidFunction) {
    SemanticModel model = analyzeToy("func f() -> void {}");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId root = res->hir.root();
    ASSERT_EQ(res->hir.kind(root), HirKind::Module);
    auto decls = res->hir.moduleDecls(root);
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::Function);
    EXPECT_EQ(res->hir.kind(res->hir.functionBody(decls[0])), HirKind::Block);
}

TEST(HirLoweringToy, ReturnLiteralPopulatesPool) {
    SemanticModel model = analyzeToy("func f() -> int { return 42; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(v.value));
    EXPECT_EQ(std::get<std::int64_t>(v.value), 42);
    EXPECT_EQ(v.core, TypeKind::I32);
}

TEST(HirLoweringToy, ArithmeticAndParams) {
    SemanticModel model = analyzeToy("func add(a : int, b : int) -> int { return a + b; }");
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

TEST(HirLoweringToy, UnaryNegation) {
    SemanticModel model = analyzeToy("func f(x : int) -> int { return -x; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ret  = res->hir.children(body)[0];
    EXPECT_EQ(res->hir.kind(*res->hir.returnValue(ret)), HirKind::UnaryOp);
}

TEST(HirLoweringToy, ControlFlow) {
    SemanticModel model = analyzeToy(
        "func f(n : int) -> void {\n"
        "  while (0 < n) {\n"
        "    if (n < 0) { return; } else { return; }\n"
        "  }\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    auto stmts = res->hir.children(body);
    ASSERT_EQ(stmts.size(), 1u);
    ASSERT_EQ(res->hir.kind(stmts[0]), HirKind::WhileStmt);
    HirNodeId loopBody = res->hir.loopBody(stmts[0]);
    EXPECT_EQ(res->hir.kind(res->hir.children(loopBody)[0]), HirKind::IfStmt);
}

TEST(HirLoweringToy, ValuelessReturn) {
    SemanticModel model = analyzeToy("func f() -> void { return; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ret  = res->hir.children(body)[0];
    ASSERT_EQ(res->hir.kind(ret), HirKind::ReturnStmt);
    EXPECT_FALSE(res->hir.returnValue(ret).has_value());        // bare `return;`
}

TEST(HirLoweringToy, CallExpression) {
    SemanticModel model = analyzeToy(
        "func g(a : int) -> int { return a; }\n"
        "func f(x : int) -> int { return g(x); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 2u);
    HirNodeId fbody = res->hir.functionBody(decls[1]);
    HirNodeId ret   = res->hir.children(fbody)[0];
    EXPECT_EQ(res->hir.kind(*res->hir.returnValue(ret)), HirKind::Call);
}

TEST(HirLoweringToy, GlobalVariable) {
    SemanticModel model = analyzeToy("var counter : int = 0;\nfunc f() -> int { return counter; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 2u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::Global);
    EXPECT_EQ(res->hir.kind(decls[1]), HirKind::Function);
}

TEST(HirLoweringToy, ExprStmtCall) {
    SemanticModel model = analyzeToy(
        "func g(a : int) -> int { return a; }\n"
        "func f() -> void { g(1); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fbody = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[1]);
    HirNodeId stmt  = res->hir.children(fbody)[0];
    ASSERT_EQ(res->hir.kind(stmt), HirKind::ExprStmt);
    EXPECT_EQ(res->hir.kind(res->hir.exprStmtExpr(stmt)), HirKind::Call);
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

TEST(HirLoweringToy, GoldenRepresentativeProgram) {
    SemanticModel model = analyzeToy(
        "var threshold : int = 10;\n"
        "func add(a : int, b : int) -> int {\n"
        "  return a + b;\n"
        "}\n"
        "func compute(n : int) -> int {\n"
        "  var doubled : int = n * 2;\n"
        "  if (doubled < threshold) {\n"
        "    return doubled;\n"
        "  } else {\n"
        "    return add(doubled, threshold);\n"
        "  }\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    // Source provenance IS populated (asserted below) but omitted from the
    // golden: BufferId is a process-global monotonic id, so `@loc(buf N, …)`
    // would depend on how many buffers earlier tests minted.
    EXPECT_GT(res->sourceMap.size(), 0u) << "lowering did not populate source provenance";

    std::vector<std::string> names = symbolNames(model);
    HirTextContext ctx;
    ctx.interner    = &model.lattice().interner();
    ctx.symbolNames = &names;
    ctx.literalPool = &res->literalPool;
    DiagnosticReporter er;
    std::string const out = emitHir(res->hir, ctx, er);

    fs::path golden = findLoweringGoldens() / "toy_compute.dsshir";
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
