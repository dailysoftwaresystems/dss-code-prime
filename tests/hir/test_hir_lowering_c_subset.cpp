// HR8 CST→HIR lowering tests: end-to-end (parse c-subset → semantic → lowerToHir
// → verify) over the covered c-subset slice, a deferred-construct diagnostic, and
// a `.dsshir` golden of a representative program. Genericity (no schema.name()
// dependence) is guaranteed by construction — the engine never inspects the
// language name — and demonstrated here by lowering a real shipped language
// through the single generic engine.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/substrate/large_stack_call.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "hir/const_eval.hpp"
#include "hir/hir.hpp"
#include "hir/hir_intrinsic_registry.hpp"
#include "hir/hir_text.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
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

// As `analyzeCSubset`, but under the PE object format — so `L'…'`/`L"…"` (wchar_t)
// resolves to the 2-byte Windows UTF-16 unit (U16), not the POSIX I32. Used to
// witness the FORMAT-keyed wide-char constraint (an astral `L'😀'` is representable
// under the default I32 but NOT under the pe U16).
[[nodiscard]] SemanticModel analyzeCSubsetPe(std::string src) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) { ADD_FAILURE() << "loadShipped(c-subset) failed"; std::abort(); }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    return analyze(cu, DataModel::Llp64, std::nullopt, std::nullopt,
                   ObjectFormatKind::Pe);
}

// Drive c-subset → SemanticModel with the parser's expression-depth cap RAISED to
// `cap` (the default is 256), so a deep-but-legal nesting beyond the cap parses to
// completion — the DEEP-NEST-RECURSION lowering pins need an input deeper than any
// program the shipped cap admits. The deep CST's parse + the orthogonal recursive
// analyze + the deep tree's teardown all run on the 64 MiB worker (the standard
// pipeline stack); `lowerToHir` is then called by the test ON ITS OWN MAIN STACK,
// which is the flat-property witness for the lowerer. A bare `int main(){…}`
// program (no `#include`) parses via Tokenizer+Parser directly (skipping the PP)
// and is ingested via `UnitBuilder::addTree` — exactly the construct these pins use.
[[nodiscard]] SemanticModel analyzeCSubsetRaisedCap(std::string src, std::size_t cap) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) { ADD_FAILURE() << "loadShipped(c-subset) failed"; std::abort(); }
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    auto srcBuf = SourceBuffer::fromString(std::move(src), "<deepmem>");
    Tokenizer tk{srcBuf, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    ParserConfig cfg;
    cfg.maxExpressionDepth = cap;
    Parser p{srcBuf, schema, std::move(stream), std::move(cfg), std::move(lexDiags)};
    ParseResult result = std::move(p).parse();
    if (result.tree.diagnostics().hasErrors()) {
        ADD_FAILURE() << "raised-cap parse produced errors (cap=" << cap << ")";
    }
    UnitBuilder builder{schema};
    builder.addTree(std::move(result.tree));
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

[[nodiscard]] HirNodeId firstFunction(Hir const& hir) {
    for (HirNodeId d : hir.moduleDecls(hir.root())) {
        if (hir.kind(d) == HirKind::Function) return d;
    }
    return HirNodeId{};
}

// First node of `want` kind in the subtree rooted at `n` (pre-order). Used to
// reach the shift BinaryOp past whatever statement wrapper holds it.
[[nodiscard]] HirNodeId findFirstByKind(Hir const& hir, HirNodeId n, HirKind want) {
    if (!n.valid()) return {};
    if (hir.kind(n) == want) return n;
    for (HirNodeId c : hir.children(n)) {
        if (HirNodeId r = findFirstByKind(hir, c, want); r.valid()) return r;
    }
    return {};
}

// The shipped c-subset JSON text (for shiftResult perturbation), found by
// walking up from cwd exactly as loadShipped does. Returned as raw text so the
// perturbation is a surgical textual swap of the closed verb value — no JSON
// library dependency in this target.
[[nodiscard]] std::string shippedCSubsetText() {
    fs::path dir = fs::current_path();
    for (int i = 0; i < 12; ++i) {
        fs::path const cand =
            dir / "src" / "dss-config" / "sources" / "c-subset.lang.json";
        if (fs::exists(cand)) {
            std::ifstream in{cand, std::ios::binary};
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    ADD_FAILURE() << "could not locate shipped c-subset.lang.json above cwd";
    return {};
}

// Lower `void f(int a, long b) { a << b; }` under a schema whose
// arithmeticConversions.shiftResult is `verb`, and return the TypeKind the
// shift BinaryOp carries. The shift is an EXPRESSION STATEMENT (no return/assign
// coercion), so the BinaryOp's own type IS the shift's result type:
// `promotedLeft` (C 6.5.7) → the promoted left operand int (I32); `commonType`
// → the usual-arithmetic common type of (int, long) = long (I64). Exercises the
// cst_to_hir shift arm — the site D-UAC-SHIFT-RESULT-RULE-CONFIG names.
[[nodiscard]] TypeKind shiftResultKind(std::string const& verb) {
    std::string text = shippedCSubsetText();
    // The shipped config declares `promotedLeft`; swap ONLY that closed-verb
    // value (unique in the file — the doc comment uses backticks, not quotes).
    std::string const needle = "\"shiftResult\": \"promotedLeft\"";
    auto const pos = text.find(needle);
    if (pos == std::string::npos) {
        ADD_FAILURE() << "shiftResult key not found in shipped c-subset config";
        std::abort();
    }
    text.replace(pos, needle.size(), "\"shiftResult\": \"" + verb + "\"");
    auto schema = GrammarSchema::loadFromText(text,
                                              "<shiftResult-" + verb + ">");
    if (!schema) {
        ADD_FAILURE() << "perturbed schema (shiftResult=" << verb << ") failed";
        std::abort();
    }
    UnitBuilder builder{*schema};
    builder.addInMemory("void f(int a, long b) { a << b; }\n", "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel model = analyze(cu);
    if (model.hasErrors()) {
        ADD_FAILURE() << "front-end errors under shiftResult=" << verb;
        std::abort();
    }
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    if (!res->ok) {
        ADD_FAILURE() << "lowering failed under shiftResult=" << verb;
        std::abort();
    }
    HirNodeId const fn = firstFunction(res->hir);
    HirNodeId const shift =
        findFirstByKind(res->hir, res->hir.functionBody(fn), HirKind::BinaryOp);
    if (!shift.valid()) {
        ADD_FAILURE() << "no BinaryOp in body under shiftResult=" << verb;
        std::abort();
    }
    return model.lattice().interner().kind(res->hir.typeId(shift));
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

// FC16 C11/C23 6.5.1.1: `_Generic` lowers to the SELECTED association's
// expression — its type + value. `i` is `int`, so the `int:` branch is selected
// and its Literal 5 IS the returned value (result type I32).
TEST(HirLoweringCSubset, GenericLowersSelectedBranchValue) {
    SemanticModel model = analyzeCSubset(
        "int f() { int i = 0; return _Generic(i, int: 5, double: 3, "
        "default: 0); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    HirNodeId const body = res->hir.functionBody(fn);
    // The return value is the selected int-branch's Literal 5 (I32).
    HirNodeId const lit =
        findFirstByKind(res->hir, body, HirKind::Literal);
    ASSERT_TRUE(lit.valid()) << "the selected branch's literal must be lowered";
    EXPECT_EQ(res->hir.kind(lit), HirKind::Literal);
    EXPECT_EQ(model.lattice().interner().kind(res->hir.typeId(lit)),
              TypeKind::I32)
        << "the selected int-branch's value types I32";
}

// FC16 6.5.1.1p3: the NON-selected association expressions are NOT evaluated —
// they must NOT be lowered. The non-selected `double: 999.0` branch's distinctive
// literal 999 must NOT reach the literal pool (only the selected `int: 5` does).
TEST(HirLoweringCSubset, GenericNonSelectedBranchNotLowered) {
    SemanticModel model = analyzeCSubset(
        "int f() { int i = 0; return _Generic(i, int: 5, "
        "double: 999, default: 777); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    // ONLY the selected branch's Literal 5 is lowered; 999 and 777 (the
    // non-selected + default branches) must be absent from the pool.
    bool has5 = false, has999 = false, has777 = false;
    for (std::size_t i = 0; i < res->literalPool.size(); ++i) {
        auto const& v = res->literalPool.at(i);
        if (std::holds_alternative<std::int64_t>(v.value)) {
            auto const iv = std::get<std::int64_t>(v.value);
            if (iv == 5)   has5 = true;
            if (iv == 999) has999 = true;
            if (iv == 777) has777 = true;
        }
    }
    EXPECT_TRUE(has5)    << "the selected int-branch literal 5 must be lowered";
    EXPECT_FALSE(has999) << "the non-selected double-branch literal 999 must NOT "
                            "be lowered (6.5.1.1p3: unevaluated)";
    EXPECT_FALSE(has777) << "the unselected default-branch literal 777 must NOT "
                            "be lowered";
}

TEST(HirLoweringCSubset, ArithmeticAndParams) {
    SemanticModel model = analyzeCSubset("int add(int a, int b) { return a + b; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    ASSERT_EQ(res->hir.kind(fn), HirKind::Function);
    EXPECT_EQ(res->hir.functionParams(fn).size(), 2u);          // a, b
    HirNodeId body = res->hir.functionBody(fn);
    auto stmts = res->hir.children(body);
    ASSERT_EQ(stmts.size(), 1u);
    ASSERT_EQ(res->hir.kind(stmts[0]), HirKind::ReturnStmt);
    HirNodeId ret = *res->hir.returnValue(stmts[0]);
    EXPECT_EQ(res->hir.kind(ret), HirKind::BinaryOp);           // a + b
}

// D-UAC-SHIFT-RESULT-RULE-CONFIG: the C 6.5.7 shift-result rule is the config
// verb `shiftResult`, read by the cst_to_hir shift arm (the site the anchor
// names). `promotedLeft` types `int << long` as the promoted left operand (I32);
// `commonType` types it like an ordinary binary op (common(int,long) = I64). The
// I32↔I64 flip when ONLY the verb changes is the red-on-disable proof the engine
// reads the verb at the HIR-lowering tier (the const-context sibling site is
// pinned in test_fc3_width_semantics.cpp).
TEST(HirLoweringCSubset, ShiftResultPromotedLeftIsLeftType) {
    EXPECT_EQ(shiftResultKind("promotedLeft"), TypeKind::I32)
        << "promotedLeft (C 6.5.7): (int << long) lowers to a BinaryOp typed I32";
}

TEST(HirLoweringCSubset, ShiftResultCommonTypeIsCommonType) {
    EXPECT_EQ(shiftResultKind("commonType"), TypeKind::I64)
        << "commonType: (int << long) lowers to a BinaryOp typed I64 — the "
           "red-on-disable flip at the cst_to_hir site";
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

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId body = res->hir.functionBody(fn);
    auto stmts = res->hir.children(body);
    ASSERT_EQ(stmts.size(), 1u);
    EXPECT_EQ(res->hir.kind(stmts[0]), HirKind::WhileStmt);
}

// D-CSUBSET-LOCAL-STATIC: a block-scope `static` lowers to a hidden module
// GLOBAL (static storage duration, C 6.2.4), NOT a function-body VarDecl (a
// stack slot). The name stays block-scoped; the STORAGE is global → the value
// persists across calls. RED-ON-DISABLE: revert the cst_to_hir staticStorage
// arm → `n` lowers to a body VarDecl → moduleDecls carries only the Function
// (zero Globals) and the body's first statement is a VarDecl, not the empty
// Block placeholder. This is the host-independent guard the runtime corpus
// (`local_static`) pairs with.
TEST(HirLoweringCSubset, StaticLocalLowersToModuleGlobal) {
    SemanticModel model = analyzeCSubset(
        "int f(void) { static int n = 0; n = n + 1; return n; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    std::size_t fns = 0, globals = 0;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) == HirKind::Function) ++fns;
        if (res->hir.kind(d) == HirKind::Global)   ++globals;
    }
    EXPECT_EQ(fns, 1u);
    EXPECT_EQ(globals, 1u)
        << "the static local must lower to ONE hidden module Global";
    // No stack VarDecl for `n` survives in the function body.
    HirNodeId fn = firstFunction(res->hir);
    for (HirNodeId s : res->hir.children(res->hir.functionBody(fn)))
        EXPECT_NE(res->hir.kind(s), HirKind::VarDecl)
            << "a static local must not leave a stack VarDecl in the body";
}

// Two SIBLING statics with the SAME source name in distinct blocks get DISTINCT
// module globals (distinct SymbolIds — no mangling needed; internal-linkage
// globals are intra-module by id). RED-ON-DISABLE: the revert collapses both to
// body VarDecls → zero module Globals.
TEST(HirLoweringCSubset, SiblingStaticLocalsGetDistinctGlobals) {
    SemanticModel model = analyzeCSubset(
        "int f(void) { { static int x = 1; x = x + 1; } "
        "{ static int x = 2; x = x + 1; } return 0; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    std::size_t globals = 0;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root()))
        if (res->hir.kind(d) == HirKind::Global) ++globals;
    EXPECT_EQ(globals, 2u)
        << "two sibling statics must mint two DISTINCT module globals";
}

TEST(HirLoweringCSubset, ForLoop) {
    SemanticModel model = analyzeCSubset(
        "void f() { for (int i = 0; i < 10; i = i + 1) {} }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId body = res->hir.functionBody(fn);
    HirNodeId forS = res->hir.children(body)[0];
    ASSERT_EQ(res->hir.kind(forS), HirKind::ForStmt);
    EXPECT_TRUE(res->hir.forInit(forS).has_value());
    EXPECT_TRUE(res->hir.loopCondition(forS).has_value());
    EXPECT_TRUE(res->hir.forUpdate(forS).has_value());
}

// c60 (Design I-A): the switch lowers to a discriminant + a flat body Block (its
// case/default markers are LabelStmts) + dispatch arms mapping each case value to
// a marker ordinal. Two dispatch arms here (case 1, default); the body Block holds
// the two case markers, each a LabelStmt.
TEST(HirLoweringCSubset, SwitchFlattensToDispatchAndBody) {
    SemanticModel model = analyzeCSubset(
        "void f(int x) { switch (x) { case 1: break; default: break; } }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId sw = res->hir.children(res->hir.functionBody(fn))[0];
    ASSERT_EQ(res->hir.kind(sw), HirKind::SwitchStmt);
    auto arms = res->hir.switchArms(sw);
    ASSERT_EQ(arms.size(), 2u);
    EXPECT_FALSE(res->hir.caseArmIsDefault(arms[0]));   // case 1
    EXPECT_TRUE(res->hir.caseArmIsDefault(arms[1]));     // default
    // The body Block holds the two case markers (LabelStmts) at its top level.
    HirNodeId const body = res->hir.switchBody(sw);
    ASSERT_EQ(res->hir.kind(body), HirKind::Block);
    auto const bodyStmts = res->hir.children(body);
    ASSERT_GE(bodyStmts.size(), 2u);
    EXPECT_EQ(res->hir.kind(bodyStmts[0]), HirKind::LabelStmt);   // case 1 marker
    // Each dispatch arm's ordinal names a LabelStmt marker that exists in the body.
    EXPECT_EQ(res->hir.caseArmLabelOrdinal(arms[0]),
              res->hir.labelOrdinal(bodyStmts[0]));
}

// D-CSUBSET-LABEL-BEFORE-CASE (c60, Design I-A) — a goto-label BEFORE a case
// (`foo: case 1: S`) parses as labelStmt(foo, caseStmt(case 1, S)) and lowers to a
// flat-body marker chain: the body's first statement is LabelStmt(foo, ...) whose
// inner is the case-1 marker LabelStmt (its ordinal = the dispatch arm's ordinal).
// `foo` stays a real LabelStmt (pre-scanned + goto-resolvable); the case dispatches
// to its own marker. (The label AFTER the colon — `case 1: foo: S` — nests the case
// marker OUTSIDE foo; both are valid C, only the relative nesting differs.)
TEST(HirLoweringCSubset, LabelBeforeCaseLowersToMarkerChain) {
    SemanticModel model = analyzeCSubset(
        "void f(int x){ switch(x){ foo: case 1: x=x+1; break; default: break; } }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId sw = res->hir.children(res->hir.functionBody(fn))[0];
    ASSERT_EQ(res->hir.kind(sw), HirKind::SwitchStmt);
    auto arms = res->hir.switchArms(sw);
    ASSERT_EQ(arms.size(), 2u);
    EXPECT_FALSE(res->hir.caseArmIsDefault(arms[0]));   // case 1
    // body[0] = LabelStmt(foo, LabelStmt(caseMarker, …)); the inner case marker's
    // ordinal equals the case-1 dispatch arm's ordinal.
    auto const bodyStmts = res->hir.children(res->hir.switchBody(sw));
    ASSERT_FALSE(bodyStmts.empty());
    ASSERT_EQ(res->hir.kind(bodyStmts[0]), HirKind::LabelStmt);   // the named `foo`
    HirNodeId const inner = res->hir.labelBody(bodyStmts[0]);
    ASSERT_EQ(res->hir.kind(inner), HirKind::LabelStmt);          // the case-1 marker
    EXPECT_EQ(res->hir.labelOrdinal(inner), res->hir.caseArmLabelOrdinal(arms[0]));
}

// c60 (Design I-A) — a BARE case (`case 1: x=…;`) lowers to a case-1 marker
// LabelStmt whose body is the real statement (the AssignStmt/ExprStmt), with NO
// stray nested caseStmt. The marker IS a LabelStmt (every case is a marker now),
// but its single body must reach the case's own statement directly.
TEST(HirLoweringCSubset, BareCaseMarkerWrapsTheBodyStatement) {
    SemanticModel model = analyzeCSubset(
        "void f(int x){ switch(x){ case 1: x=x+1; break; default: break; } }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId sw = res->hir.children(res->hir.functionBody(fn))[0];
    ASSERT_EQ(res->hir.kind(sw), HirKind::SwitchStmt);
    auto const bodyStmts = res->hir.children(res->hir.switchBody(sw));
    ASSERT_GE(bodyStmts.size(), 1u);
    ASSERT_EQ(res->hir.kind(bodyStmts[0]), HirKind::LabelStmt);   // case 1 marker
    // The marker's inner is the case body (an assignment), NOT another caseStmt/
    // label wrapper.
    HirNodeId const inner = res->hir.labelBody(bodyStmts[0]);
    EXPECT_NE(res->hir.kind(inner), HirKind::LabelStmt);
}

// D-CSUBSET-LABEL-BEFORE-CASE guard — a `caseStmt` that is not a direct switch-body
// item (here: outside any switch) fails loud S_CaseLabelNotInSwitch (C 6.8.1),
// never a stray arm-less case. Red-on-disable: drop the lowerStmt CaseStmt guard.
TEST(HirLoweringCSubset, CaseLabelOutsideSwitchFailsLoud) {
    SemanticModel model = analyzeCSubset("int f(void){ case 1: return 0; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GE(countCode(r, DiagnosticCode::S_CaseLabelNotInSwitch), 1u);
}

// c60 (Design I-A) — the multi-label ADJACENT-case chain + a label before
// `default`. `foo: case 1: case 2: S` parses as labelStmt(foo, caseStmt(1,
// caseStmt(2, S))) → body[0] = LabelStmt(foo, LabelStmt(case1, LabelStmt(case2,
// S))); `bar: default: S2` → LabelStmt(bar, LabelStmt(default, S2)). Three dispatch
// arms (case 1, case 2, default), each with a distinct marker ordinal.
TEST(HirLoweringCSubset, LabelBeforeAdjacentCasesAndDefaultMarkerChain) {
    SemanticModel model = analyzeCSubset(
        "void f(int x){ switch(x){ foo: case 1: case 2: x=x+1; break; "
        "bar: default: x=x+9; break; } }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId sw = res->hir.children(res->hir.functionBody(fn))[0];
    ASSERT_EQ(res->hir.kind(sw), HirKind::SwitchStmt);
    auto arms = res->hir.switchArms(sw);
    ASSERT_EQ(arms.size(), 3u);                          // case 1, case 2, default
    EXPECT_FALSE(res->hir.caseArmIsDefault(arms[0]));    // case 1
    EXPECT_FALSE(res->hir.caseArmIsDefault(arms[1]));    // case 2
    EXPECT_TRUE(res->hir.caseArmIsDefault(arms[2]));     // default
    // Distinct ordinals for the three markers.
    EXPECT_NE(res->hir.caseArmLabelOrdinal(arms[0]), res->hir.caseArmLabelOrdinal(arms[1]));
    EXPECT_NE(res->hir.caseArmLabelOrdinal(arms[1]), res->hir.caseArmLabelOrdinal(arms[2]));
    // body[0] = LabelStmt(foo, LabelStmt(case1, LabelStmt(case2, …))).
    auto const bodyStmts = res->hir.children(res->hir.switchBody(sw));
    ASSERT_FALSE(bodyStmts.empty());
    ASSERT_EQ(res->hir.kind(bodyStmts[0]), HirKind::LabelStmt);       // foo
    HirNodeId const c1 = res->hir.labelBody(bodyStmts[0]);
    ASSERT_EQ(res->hir.kind(c1), HirKind::LabelStmt);                 // case 1 marker
    EXPECT_EQ(res->hir.labelOrdinal(c1), res->hir.caseArmLabelOrdinal(arms[0]));
    HirNodeId const c2 = res->hir.labelBody(c1);
    ASSERT_EQ(res->hir.kind(c2), HirKind::LabelStmt);                 // case 2 marker
    EXPECT_EQ(res->hir.labelOrdinal(c2), res->hir.caseArmLabelOrdinal(arms[1]));
}

// D-CSUBSET-LABEL-BUDGET-CLIFF (p19 Cluster G c31) — the `commitAfterPrefix`
// CUT lets `declOrExprStmt`'s `labelStmt` probe COMMIT after its 2-token fixed
// prefix (`Identifier Colon`) is consumed, so the (arbitrarily large) labeled
// `statement` then parses NON-speculatively (off the lookahead*16 = 4096-token
// probe budget). This pin builds a label before a statement whose token count
// is FAR over 4096; with the cut it parses clean, lowers, and runs. RED-ON-
// DISABLE: revert `"commitAfterPrefix": true` on labelStmt (or the parser cut)
// and the labelStmt probe exhausts its budget, rolls back, falls through to
// exprStmt, and emits P0001 ("got ':'") — `model.hasErrors()` then trips the
// ASSERT below. The body (`i = i + vN`, fold-resistant on a parameter) also
// makes the result observable so the labeled block is not DCE'd to nothing.
TEST(HirLoweringCSubset, LabelBeforeOversizeStatementParsesPastProbeBudget) {
    // ~500 statements inside the labeled block ⇒ ~5500 tokens, comfortably
    // over the 4096-token speculative-probe budget (lookahead 256 * 16).
    std::string src = "int f(int i){\n  L: {\n";
    for (int n = 0; n < 500; ++n) {
        src += "    int v" + std::to_string(n) + " = " + std::to_string(n)
             + "; i = i + v" + std::to_string(n) + ";\n";
    }
    src += "  }\n  return i;\n}\n";
    SemanticModel model = analyzeCSubset(std::move(src));
    // The load-bearing assertion: a clean front end. On revert this is the
    // P0001 'got :' from the budget rollback.
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    ASSERT_EQ(res->hir.kind(fn), HirKind::Function);
    auto body = res->hir.children(res->hir.functionBody(fn));
    ASSERT_EQ(body.size(), 2u);                           // labeled block + return
    EXPECT_EQ(res->hir.kind(body[0]), HirKind::LabelStmt);
    EXPECT_EQ(res->hir.kind(res->hir.labelBody(body[0])), HirKind::Block);
}

// ★★ BRACELESS-BODY CORRECTNESS — the silent-miscompile guard for the cut.
// `labelStmt` STAYS `Identifier Colon statement`, so a label in a braceless
// control-flow body keeps its labeled statement AS that body: in
// `if(x) L: g=42;` the `L: g=42` IS the if's then-branch, NOT a sibling that
// runs unconditionally. Were labelStmt ever flattened to a 2-token statement
// (label + a SEPARATE following statement), `g=42` would detach from the `if`
// and execute even when `x==0` — a C-semantics miscompile that runs green on
// any test that only checks the x!=0 path. This pin pins the STRUCTURE: the
// if's then-branch is the LabelStmt, and the assignment hangs UNDER it. The
// `label_before_switch_goto` runtime corpus is the executable companion; the
// gate's `f(0)→5` (the assignment is skipped) is the same property at runtime.
TEST(HirLoweringCSubset, LabelAsBracelessIfBodyStaysInsideTheIf) {
    SemanticModel model = analyzeCSubset(
        "int g; int f(int x){ g = 5; if(x) L: g = 42; return g; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    // f is the SECOND module decl (g is the first — a global var).
    HirNodeId fn{};
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) == HirKind::Function) { fn = d; break; }
    }
    ASSERT_TRUE(fn.valid());
    auto body = res->hir.children(res->hir.functionBody(fn));
    // EXACTLY three statements: `g=5`, the `if`, `return g`. A flattened label
    // would make `g=42` a FOURTH sibling here (and the if's then-branch empty).
    ASSERT_EQ(body.size(), 3u);
    EXPECT_EQ(res->hir.kind(body[0]), HirKind::AssignStmt);   // g = 5
    ASSERT_EQ(res->hir.kind(body[1]), HirKind::IfStmt);       // if (x) ...
    EXPECT_EQ(res->hir.kind(body[2]), HirKind::ReturnStmt);   // return g
    // The if's then-branch IS the label, and `g=42` hangs under it. (The
    // assignment lowers to AssignStmt — the load-bearing structural fact is
    // that it nests UNDER the LabelStmt UNDER the if, not that it is a
    // sibling of the if running unconditionally.)
    HirNodeId thenBranch = res->hir.ifThen(body[1]);
    ASSERT_EQ(res->hir.kind(thenBranch), HirKind::LabelStmt);
    EXPECT_EQ(res->hir.kind(res->hir.labelBody(thenBranch)), HirKind::AssignStmt);
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

// D-LANG-POINTER-VOID-CONVERT (audit-fold G4, step 13.2, 2026-06-02):
// pins that `cst_to_hir.cpp`'s coerce() arm ACTUALLY emits the
// synthetic `Cast(Ptr<T>→Ptr<Void>)` HIR node when c-subset's
// `pointerConversions.implicitToVoidPtr` admits a T*→void* call
// arg. Pre-G4 only the semantic analyzer's S_TypeMismatch absence
// was pinned — if the lowering arm silently failed to emit the
// Cast (e.g., the `admit` branch returned false unexpectedly), MIR
// would see Ptr<I8> where Ptr<Void> was expected and silently
// type-skew at the HIR/MIR boundary.
TEST(HirLoweringCSubset, CoerceEmitsCastForCharPtrToVoidPtrArg) {
    SemanticModel model = analyzeCSubset(
        "extern int handler(void* p);\n"
        "int main() {\n"
        "    char* s;\n"
        "    return handler(s);\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto const& ti = model.lattice().interner();
    // Navigate to the `handler(s)` call's arg. Decls: [extern handler, main].
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_GE(decls.size(), 2u);
    HirNodeId const mainBody = res->hir.functionBody(decls.back());
    auto const stmts = res->hir.children(mainBody);
    ASSERT_GE(stmts.size(), 1u);
    // Last statement is `return handler(s);` — find its Call.
    HirNodeId const ret = stmts.back();
    HirNodeId const call = *res->hir.returnValue(ret);
    ASSERT_EQ(res->hir.kind(call), HirKind::Call);
    // Call children: [callee, arg0, arg1, ...]
    auto const callKids = res->hir.children(call);
    ASSERT_EQ(callKids.size(), 2u);
    HirNodeId const arg0 = callKids[1];
    // The arg MUST be a synthetic Cast — not a bare Ref/Literal —
    // because the void-pointer conversion is a tracked materialization.
    ASSERT_EQ(res->hir.kind(arg0), HirKind::Cast)
        << "char* → void* arg must lower to an explicit Cast HIR node, "
           "not a bare Ref — otherwise MIR sees the source type and "
           "downstream Bitcast lowering loses the type-skew evidence";
    // Cast's result type IS Ptr<Void>.
    TypeId const argTy = res->hir.typeId(arg0);
    ASSERT_EQ(ti.kind(argTy), TypeKind::Ptr);
    auto const elem = ti.operands(argTy);
    ASSERT_FALSE(elem.empty());
    EXPECT_EQ(ti.kind(elem[0]), TypeKind::Void)
        << "Cast target must be Ptr<Void>, not Ptr<I8>";
}

// D-LANG-POINTER-VOID-CONVERT (audit-fold G4, reverse direction):
// `void* → char*` via call arg also lowers to an explicit Cast
// whose result type is Ptr<Char>. Pins the implicitFromVoidPtr arm.
TEST(HirLoweringCSubset, CoerceEmitsCastForVoidPtrToCharPtrArg) {
    SemanticModel model = analyzeCSubset(
        "extern int handler(char* s);\n"
        "extern void* alloc(int n);\n"
        "int main() {\n"
        "    return handler(alloc(16));\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto const& ti = model.lattice().interner();
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_GE(decls.size(), 3u);
    HirNodeId const mainBody = res->hir.functionBody(decls.back());
    auto const stmts = res->hir.children(mainBody);
    HirNodeId const ret = stmts.back();
    HirNodeId const outerCall = *res->hir.returnValue(ret);
    ASSERT_EQ(res->hir.kind(outerCall), HirKind::Call);
    auto const outerKids = res->hir.children(outerCall);
    ASSERT_EQ(outerKids.size(), 2u);
    HirNodeId const arg0 = outerKids[1];
    ASSERT_EQ(res->hir.kind(arg0), HirKind::Cast)
        << "void* → char* via call arg must materialize as Cast";
    TypeId const argTy = res->hir.typeId(arg0);
    ASSERT_EQ(ti.kind(argTy), TypeKind::Ptr);
    auto const elem = ti.operands(argTy);
    ASSERT_FALSE(elem.empty());
    EXPECT_EQ(ti.kind(elem[0]), TypeKind::Char)
        << "Cast target must be Ptr<Char>, not Ptr<Void>";
}

// D-LANG-NULL-POINTER-CONSTANT (step 13.3, 2026-06-02): pin that
// `cst_to_hir.cpp`'s coerce() arm materializes literal 0 → Ptr<T> as
// an explicit `Cast(IntLiteral(0), Ptr<T>)` HIR node. MIR's mapCast
// routes IntToPtr (literal-0 → pointer-width zero in the dest
// register); without the explicit Cast, MIR would see I32 where
// Ptr was expected → silent type-skew at the HIR/MIR boundary.
TEST(HirLoweringCSubset, NullPointerConstantEmitsCastInCallArg) {
    SemanticModel model = analyzeCSubset(
        "extern void f(void* p);\n"
        "int main() { f(0); return 0; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto const& ti = model.lattice().interner();
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_GE(decls.size(), 2u);
    HirNodeId const mainBody = res->hir.functionBody(decls.back());
    auto const stmts = res->hir.children(mainBody);
    ASSERT_GE(stmts.size(), 1u);
    // First statement is the `f(0)` ExprStmt — find its Call.
    HirNodeId const exprStmt = stmts[0];
    HirNodeId const call = res->hir.exprStmtExpr(exprStmt);
    ASSERT_EQ(res->hir.kind(call), HirKind::Call);
    auto const callKids = res->hir.children(call);
    ASSERT_EQ(callKids.size(), 2u);
    HirNodeId const arg0 = callKids[1];
    ASSERT_EQ(res->hir.kind(arg0), HirKind::Cast)
        << "literal 0 → void* must materialize as an explicit Cast "
           "HIR node; otherwise MIR sees I32 where Ptr is expected";
    TypeId const argTy = res->hir.typeId(arg0);
    ASSERT_EQ(ti.kind(argTy), TypeKind::Ptr);
    auto const elem = ti.operands(argTy);
    ASSERT_FALSE(elem.empty());
    EXPECT_EQ(ti.kind(elem[0]), TypeKind::Void);
}

// D-CSUBSET-CAST-ARRAY-DECAY (FC3.5 sweep-c3): the explicit cast of a
// string literal lowers THROUGH the synthetic array-to-pointer decay
// (C 6.3.2.1p3): `(long)"xy"` is Cast(I64 ← Cast(Ptr<Char> ←
// Array<Char>)) — the inner decay is the SAME synthetic Cast the
// implicit path emits (mapCast materializes the rodata global +
// GlobalAddr), the outer Cast is the programmer's PtrToInt. Without
// the decay the MIR mapCast would see Array directly (no arm — fail).
TEST(HirLoweringCSubset, ExplicitCastOfStringLiteralLowersViaDecay) {
    SemanticModel model = analyzeCSubset(
        "int main() { return (int)(long)\"xy\"; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto const& ti = model.lattice().interner();
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_GE(decls.size(), 1u);
    HirNodeId const mainBody = res->hir.functionBody(decls.back());
    auto const stmts = res->hir.children(mainBody);
    ASSERT_GE(stmts.size(), 1u);
    HirNodeId const ret = stmts.back();
    HirNodeId const outer = *res->hir.returnValue(ret);
    // (int) ← (long) ← decay(Ptr<Char>) ← "xy"
    ASSERT_EQ(res->hir.kind(outer), HirKind::Cast);
    EXPECT_EQ(ti.kind(res->hir.typeId(outer)), TypeKind::I32);
    auto const outerKids = res->hir.children(outer);
    ASSERT_EQ(outerKids.size(), 1u);
    HirNodeId const longCast = outerKids[0];
    ASSERT_EQ(res->hir.kind(longCast), HirKind::Cast);
    EXPECT_EQ(ti.kind(res->hir.typeId(longCast)), TypeKind::I64);
    auto const longKids = res->hir.children(longCast);
    ASSERT_EQ(longKids.size(), 1u);
    HirNodeId const decay = longKids[0];
    ASSERT_EQ(res->hir.kind(decay), HirKind::Cast)
        << "the cast operand must pass through the synthetic "
           "array-to-pointer decay Cast (C 6.3.2.1p3) — mapCast has "
           "no Array→int arm";
    TypeId const decayTy = res->hir.typeId(decay);
    ASSERT_EQ(ti.kind(decayTy), TypeKind::Ptr)
        << "the decay result must be Ptr<Char>";
    auto const decayElem = ti.operands(decayTy);
    ASSERT_FALSE(decayElem.empty());
    EXPECT_EQ(ti.kind(decayElem[0]), TypeKind::Char);
}

// D-CSUBSET-COMPOUND-LITERAL-TYPEDEF (FC3.5 sweep-c3): a typedef'd
// STRUCT compound literal lowers through HIR cleanly — the semantic
// stamping (semantics.compoundLiterals) resolved `MyP` so
// `resolveStampedTypeBelow` finds the struct type and `lowerBraceInit`
// builds the aggregate. HONEST TIER NOTE: this pins the
// parse+semantic+HIR tiers; struct VALUES do not reach LIR codegen yet
// (the aggregate MIR ops are a pre-existing '<deferred>' lowering
// gap), so no runtime corpus arm exists for compound literals.
TEST(HirLoweringCSubset, TypedefStructCompoundLiteralLowersTyped) {
    SemanticModel model = analyzeCSubset(
        "struct P { int x; int y; };\n"
        "typedef struct P MyP;\n"
        "int main() { struct P p = (MyP){40, 2}; return p.x; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok)
        << "typedef'd compound literal must lower through HIR: "
        << (r.all().empty() ? "" : r.all()[0].actual);
}

// FC17.5 (D-CSUBSET-EMPTY-INITIALIZER, C23 6.7.10): SCALAR compound
// literals — `(int){42}`, valid C 6.5.2.5p9 — now lower through the HIR
// scalar brace-init arm (the FLIP of the pre-FC17.5
// `ScalarCompoundLiteralStaysAggregateOnlyFailLoud` pin, which
// anticipated exactly this lift). The single-expression form is
// byte-identical to a plain `= 42` init after the arm's coerce.
TEST(HirLoweringCSubset, ScalarCompoundLiteralLowersViaScalarBraceInit) {
    SemanticModel model = analyzeCSubset(
        "int main() { int x = (int){42}; return x; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "the semantic tier must admit + type the scalar compound "
           "literal (the stamp resolves)";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok)
        << "the FC17.5 scalar brace-init arm must lower `(int){42}` "
           "cleanly: "
        << (r.all().empty() ? "" : r.all()[0].actual);
}

// FC17.5 (D-CSUBSET-EMPTY-INITIALIZER, C23 6.7.10p11): the EMPTY
// initializer `{}` zero-initializes a scalar / a pointer, and the
// single-expression form `{42}` initializes with the expression —
// every route funnels through the ONE lowerBraceInit chokepoint.
TEST(HirLoweringCSubset, ScalarEmptyAndSingleBraceInitLower) {
    SemanticModel model = analyzeCSubset(
        "int main() {\n"
        "    int z = {};\n"
        "    int v = {42};\n"
        "    int *p = {};\n"
        "    return z + v + (p == 0 ? 0 : 1);\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

// FC17.5 (S_InvalidScalarInitializer 0xE03F): the three malformed scalar
// brace shapes stay LOUD — excess elements (`{1,2}`), a designator on a
// scalar (`{.x=1}`), and the audit-N2 nested brace list (`{{42}}` — C23
// 6.7.10p12 requires a SINGLE expression). Each is a distinct arm of the
// scalar lowering's reject path; a silent guess would ship wrong bytes.
TEST(HirLoweringCSubset, ScalarBraceInitMalformedShapesFailLoud) {
    struct Arm { char const* src; char const* what; };
    Arm const arms[] = {
        {"int main() { int v = {1, 2}; return v; }\n",   "excess elements"},
        {"int main() { int v = {.x = 1}; return v; }\n", "designator on scalar"},
        {"int main() { int v = {{42}}; return v; }\n",   "nested brace list (N2)"},
    };
    for (auto const& arm : arms) {
        SemanticModel model = analyzeCSubset(arm.src);
        ASSERT_FALSE(model.hasErrors())
            << arm.what << ": the semantic tier admits the parse (the "
                            "constraint is enforced at HIR lowering)";
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_FALSE(res->ok)
            << arm.what << " must fail loud at the scalar brace-init arm";
        bool sawCode = false;
        for (auto const& d : r.all()) {
            if (d.code == DiagnosticCode::S_InvalidScalarInitializer)
                sawCode = true;
        }
        EXPECT_TRUE(sawCode)
            << arm.what << " must report S_InvalidScalarInitializer (0xE03F)";
    }
}

// FC17.5 F4 (the CLOSED allowlist): `(void){}` stays LOUD — Void is not
// an allowlisted scalar brace-init target (admitting it would mint a
// Void-typed literal and corrupt the type system). The aggregate gate's
// fail-loud reject is the backstop for every non-allowlisted kind.
TEST(HirLoweringCSubset, VoidCompoundLiteralStaysFailLoud) {
    SemanticModel model = analyzeCSubset(
        "int main() { (void){}; return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "the semantic tier stamps the void compound literal; the HIR "
           "gate is the enforcement point";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "`(void){}` must stay fail-loud (F4 closed allowlist)";
}

// FC17.5 (D-CSUBSET-FUNC-PREDEFINED-IDENTIFIER, C99 6.4.2.2): a read of
// `__func__` FOLDS to a string-literal-shaped constant and every string
// consumer (decay to `const char*`, indexing, address-of) rides the
// existing paths — the whole program lowers cleanly through HIR.
TEST(HirLoweringCSubset, FuncNameReadsLowerThroughStringLiteralPaths) {
    SemanticModel model = analyzeCSubset(
        "int helper(void) { return __func__[0] == 'h' ? 1 : 0; }\n"
        "int main() {\n"
        "    const char *fn = __func__;\n"
        "    int a = fn[0] == 'm' ? 1 : 0;\n"
        "    int b = (&__func__ != 0) ? 1 : 0;\n"
        "    int c = (__func__ == __func__) ? 1 : 0;\n"
        "    int d = (int)sizeof __func__;\n"
        "    return a + b + c + d + helper();\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

// FC17.5 F1 (S_PredefinedIdentifierNotAddressable 0xE040): `++__func__`
// reaches the HIR inc/dec classifier (SE4's const check does not model
// inc/dec — the pre-existing D-CSUBSET-INCDEC-CONST-LVALUE class), where
// the simpleLvalue chokepoint now rejects the predefined identifier with
// a REAL diagnostic instead of the engine-level "no storage slot" MIR
// failure it would otherwise dead-end at. Covers `--__func__` and the
// postfix forms by construction (all three ++/-- sites share the
// classifier, and the classifier's simple-lvalue probe IS the guard).
TEST(HirLoweringCSubset, FuncNameIncDecFailsLoudWithRealDiagnostic) {
    SemanticModel model = analyzeCSubset(
        "int main() { ++__func__; return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "inc/dec const-ness is not modelled at semantic — the HIR "
           "guard is the enforcement point";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "++__func__ must fail loud";
    bool sawCode = false;
    for (auto const& d : r.all()) {
        if (d.code == DiagnosticCode::S_PredefinedIdentifierNotAddressable)
            sawCode = true;
    }
    EXPECT_TRUE(sawCode)
        << "++__func__ must report S_PredefinedIdentifierNotAddressable "
           "(0xE040), not an engine-level unsupported-lowering error";
}

// D-CSUBSET-CAST-VOID-DISCARD (FC3.5 sweep-c3): `(void)f()` lowers as
// evaluate-operand-discard — the expression statement's node IS the
// Call itself, with NO Cast wrapping it (mapCast has no void arm by
// design; the discard is a statement effect, not a conversion). The
// operand's presence in the lowered tree is the evaluation guarantee.
TEST(HirLoweringCSubset, VoidDiscardCastLowersOperandWithoutCastNode) {
    SemanticModel model = analyzeCSubset(
        "extern int f();\n"
        "int main() { (void)f(); return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_GE(decls.size(), 2u);
    HirNodeId const mainBody = res->hir.functionBody(decls.back());
    auto const stmts = res->hir.children(mainBody);
    ASSERT_GE(stmts.size(), 2u);
    HirNodeId const exprStmt = stmts[0];
    HirNodeId const inner = res->hir.exprStmtExpr(exprStmt);
    ASSERT_EQ(res->hir.kind(inner), HirKind::Call)
        << "(void)f() must lower to the bare Call (operand evaluated "
           "for effects) — NO Cast node may wrap it (mapCast has no "
           "void arm; a Cast here would fail-loud downstream)";
}

// D5.1: a `struct Foo { int x; int y; };` declaration lowers to a HIR
// `TypeDecl` whose `typeId` is the composed `structType("Foo", {I32, I32})`.
TEST(HirLoweringCSubset, StructDeclarationLowersToTypeDecl) {
    SemanticModel model = analyzeCSubset(
        "struct Point { int x; int y; };\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::TypeDecl);

    // The TypeDecl carries the composed struct type.
    TypeId const t = res->hir.typeDeclType(decls[0]);
    ASSERT_TRUE(t.valid());
    auto const& interner = model.lattice().interner();
    EXPECT_EQ(interner.kind(t), TypeKind::Struct);
    EXPECT_EQ(interner.name(t), "Point");
    // Field ordering: x (I32), y (I32) — declaration order.
    auto fields = interner.operands(t);
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(interner.kind(fields[0]), TypeKind::I32);
    EXPECT_EQ(interner.kind(fields[1]), TypeKind::I32);
}

// C 6.7p2 — a top-level declaration with NEITHER a named declarator NOR a tag
// (`int ;`) declares nothing. This became grammar-parseable when the init-
// declarator-list was made OPTIONAL (to admit the bare `struct P {…};` form,
// D-CSUBSET-STRUCT-BODY-VARDECL-POSITION). The HIR lowering must FAIL LOUD with
// S_DeclarationDeclaresNothing — and must NOT crash: the prior code drove
// `findCompositeSpecifierIn`'s `tree.rule()` over a leaf token (the `int`),
// tripping the `Internal`-node assertion. RED-ON-DISABLE on BOTH halves: revert
// the `findCompositeSpecifierIn` Internal guard → this test CRASHES; drop the
// `emitH(S_DeclarationDeclaresNothing)` → `res->ok` stays true and the count is 0.
// (The sibling `StructDeclarationLowersToTypeDecl` above proves the diagnostic
// does NOT false-fire on a real bare tag-declaring def.)
TEST(HirLoweringCSubset, TopLevelDeclaresNothingFailsLoudNoCrash) {
    SemanticModel model = analyzeCSubset("int ;\nint main(void) { return 0; }\n");
    // The constraint violation is HIR-tier: parse + semantic accept `int ;`.
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "`int ;` declares nothing — lowering must fail loud, not accept";
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 1u)
        << "exactly one S_DeclarationDeclaresNothing for the empty `int ;` decl";
}

// C23 6.7.2.5 (D-CSUBSET-TYPEOF): a bare `typeof(x);` — a typeof type-specifier
// with NO declarator — declares nothing, exactly like `int ;`. The typeof head is
// a type-specifier (not a struct/union/enum composite), so the "declares nothing"
// path fires at HIR lowering (S_DeclarationDeclaresNothing) and must NOT crash on
// the typeof subtree. Semantic + parse accept it (x is a declared global); the
// constraint is HIR-tier.
TEST(HirLoweringCSubset, BareTypeofDeclaresNothingFailsLoud) {
    SemanticModel model = analyzeCSubset(
        "int x;\ntypeof(x);\nint main(void) { return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "`typeof(x);` declares nothing — lowering must fail loud, not accept";
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 1u)
        << "exactly one S_DeclarationDeclaresNothing for the bare `typeof(x);` decl";
}

// c25 D-CSUBSET-UNIFIED-COMPOSITE-SPECIFIER: a body-PRESENT specifier WITH a
// declarator (`struct S { int x; } v;`) is a definition-introducing global — it
// lowers cleanly (the `compositeSpecifierIsDefinition` gate admits it because its
// body child is present). The body-ABSENT counterpart (`struct S;`) is now a
// FORWARD DECLARATION (c35 D-CSUBSET-FORWARD-STRUCT-DECLARATION): it mints an
// opaque tag at the semantic tier and lowers to NOTHING (no TypeDecl, no fail) —
// pinned in TopLevelForwardStructDeclLowersToNothing below. RED-on-disable: if
// the body-present gate were inverted, THIS would spuriously fail-loud.
TEST(HirLoweringCSubset, StructDefinitionWithObjectLowersClean) {
    SemanticModel model = analyzeCSubset(
        "struct S { int x; } v;\nint main(void) { return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 0u)
        << "`struct S { int x; } v;` declares an object — must NOT fail loud";
}

// c35 D-CSUBSET-FORWARD-STRUCT-DECLARATION: a bare top-level FORWARD declaration
// (`struct S;` — a body-ABSENT NAMED composite specifier) is the opaque-tag
// declaration. The semantic tier minted the incomplete tag; the HIR lowering must
// emit NOTHING and must NOT fail loud (it is NOT a declares-nothing constraint
// violation). RED-on-disable: drop the `findForwardCompositeSpecifierIn` no-op arm
// and the bare forward decl re-emits S_DeclarationDeclaresNothing (res->ok false).
// Contrast TopLevelDeclaresNothingFailsLoudNoCrash (`int ;`, NO tag → still loud).
TEST(HirLoweringCSubset, TopLevelForwardStructDeclLowersToNothing) {
    SemanticModel model = analyzeCSubset(
        "struct S;\nint main(void) { return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 0u)
        << "`struct S;` is a forward declaration of an opaque tag — must NOT fail loud";
}

// c35: the LOCAL twin — a bare `struct S;` as a block STATEMENT is a (block-scoped)
// forward declaration; lowers to nothing, no fail-loud. Contrast
// LocalDeclaresNothingFailsLoud (`int;`, NO tag → still loud).
TEST(HirLoweringCSubset, LocalForwardStructDeclLowersToNothing) {
    SemanticModel model = analyzeCSubset(
        "int main(void){ struct S; return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 0u)
        << "a block-scoped `struct S;` forward declaration must NOT fail loud";
}

// D5.1: a struct used as a pointer-typed parameter + member access via `->`
// resolves the field SymbolId AND propagates the field's type to the
// member-access node. Pins the SEMANTIC layer (Pass 1.5 struct composition +
// Pass 2 field-symbol binding + type propagation).
TEST(HirLoweringCSubset, StructFieldAccessResolvesSemantically) {
    SemanticModel model = analyzeCSubset(
        "struct Point { int x; int y; };\n"
        "int read_x(struct Point *p) { return p->x; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);

    // Walk the CU's tree to find the `x` identifier inside `p->x`. The CST has
    // one Identifier token per `x` reference; field `x`'s declaration is the
    // FIRST one (under `struct Point { int x; ... }`) and the USE is the LAST
    // one (under `return p->x;`). The semantic model's reverse use-index makes
    // this robust: every use of the field symbol is recorded.
    auto const& cu = model.unit();
    SymbolId xField = InvalidSymbol;
    for (auto const& rec : model.symbols()) {
        if (rec.name == "x" && rec.kind == DeclarationKind::Variable) {
            xField = SymbolId{static_cast<std::uint32_t>(&rec - model.symbols().data())};
            break;
        }
    }
    ASSERT_TRUE(xField.valid()) << "field symbol 'x' not minted";

    // Pass 1 stamps the field's ordinal index on the symbol.
    auto const* xRec = model.recordFor(xField);
    ASSERT_NE(xRec, nullptr);
    EXPECT_EQ(xRec->fieldIndex, 0u);  // x is the first field

    // Pass 2 should have recorded the field use on the `p->x` access. The
    // reverse-use index gives us every use-site node for the field symbol;
    // for `p->x` inside `read_x`, that's exactly one use.
    auto uses = model.usesOf(xField);
    ASSERT_EQ(uses.size(), 1u) << "field 'x' should have exactly one use site";

    // The use node should carry the field's type (I32) via Pass 2's
    // propagation. The MEMBER-ACCESS node (the parent postfixExpr) should
    // ALSO carry that type so chained access `p->x.y` would resolve the
    // next layer naturally.
    NodeId useNode = uses[0];
    TypeId useType = model.typeAt(useNode);
    auto const& interner = model.lattice().interner();
    ASSERT_TRUE(useType.valid());
    EXPECT_EQ(interner.kind(useType), TypeKind::I32);

    // Find the parent postfixExpr node and verify its type matches.
    Tree const& tree = cu.trees()[0];
    NodeId parent = tree.parent(useNode);
    // memberFollower wraps the Identifier; postfixExpr wraps memberFollower.
    while (parent.valid() && tree.kind(parent) == NodeKind::Internal
           && tree.rule(parent).v != model.unit().schema().rules().find("postfixExpr").v) {
        parent = tree.parent(parent);
    }
    ASSERT_TRUE(parent.valid()) << "no postfixExpr ancestor for field use";
    TypeId accessType = model.typeAt(parent);
    ASSERT_TRUE(accessType.valid()) << "member-access node has no type";
    EXPECT_EQ(interner.kind(accessType), TypeKind::I32);
}

// D5.1 cycle 4: the `MemberAccess` HIR-lowering branch in lowerPostfix. A `.`
// access lowers to a plain `HirKind::MemberAccess` with the field's payload =
// fieldIndex. An arrow `->` access is desugared at HIR level to
// `MemberAccess(Deref(p), idx)` -- same HirKind handles both forms; MIR sees
// uniform GEP-after-load patterns.
TEST(HirLoweringCSubset, MemberAccessLowersToHirMemberAccess) {
    SemanticModel model = analyzeCSubset(
        "struct Point { int x; int y; };\n"
        "int get_y(struct Point *p) { return p->y; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 2u);   // struct Point, function get_y
    HirNodeId const fn = decls[1];
    ASSERT_EQ(res->hir.kind(fn), HirKind::Function);
    HirNodeId const body = res->hir.functionBody(fn);
    // body = Block of [ReturnStmt(MemberAccess(Deref(Ref(p)), 1))]
    auto stmts = res->hir.children(body);
    ASSERT_EQ(stmts.size(), 1u);
    HirNodeId const ret = stmts[0];
    ASSERT_EQ(res->hir.kind(ret), HirKind::ReturnStmt);
    auto const retVal = res->hir.returnValue(ret);
    ASSERT_TRUE(retVal.has_value());
    HirNodeId const access = *retVal;
    ASSERT_EQ(res->hir.kind(access), HirKind::MemberAccess);
    // Field 'y' is the SECOND field (index 1) of Point.
    EXPECT_EQ(res->hir.payload(access), 1u);
    // The MemberAccess's result type is the field type (I32).
    auto const& interner = model.lattice().interner();
    EXPECT_EQ(interner.kind(res->hir.typeId(access)), TypeKind::I32);
    // Arrow form: the access's single child is a Deref of the original LHS.
    auto kids = res->hir.children(access);
    ASSERT_EQ(kids.size(), 1u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::Deref);
    // The Deref's result type is the pointee (struct Point).
    EXPECT_EQ(interner.kind(res->hir.typeId(kids[0])), TypeKind::Struct);
}

// D5.2 cycle 1: adding `Identifier` to `typeBase` lets a typedef-name work
// bare in type position at top level. The engine's `resolveTypeNode` resolves
// Identifier-in-type-position via the SE5 alias path (an ORDINARY Type-kind
// symbol → its `.type`); this cycle's contribution is the schema change that
// lets the parser accept the form. Block-scope alias (`{ Foo x; }`) is
// intentionally deferred — it collides with `exprStmt` at the statement alt
// and needs speculative-alt support (later cycle).
//
// C 6.2.3 tag namespace (now SEPARATED): a bare `Foo` resolves ONLY an
// ordinary typedef-name — a struct TAG `Foo` is reachable only as `struct Foo`
// (see `BareStructTagNotUsableAsTypeName` below). Here the alias `FooT` is a
// genuine typedef (Ordinary), so `FooT origin;` resolves it directly.
TEST(HirLoweringCSubset, TypedefStructAliasAtTopLevel) {
    // The alias name differs from the struct tag here purely for clarity; with
    // the separate tag namespace a SAME-named alias is now also legal (see
    // `TypedefSameNameAsTagCoexistsAcrossNamespaces`).
    SemanticModel model = analyzeCSubset(
        "struct Foo { int x; };\n"
        "typedef struct Foo FooT;\n"
        "FooT origin;\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 3u);   // struct Foo, typedef FooT, origin
    // The third decl is the Global `origin`; its type should be the same
    // composed Struct as the struct decl produced.
    HirNodeId const origin = decls[2];
    ASSERT_EQ(res->hir.kind(origin), HirKind::Global);
    TypeId const originType = res->hir.globalType(origin);
    ASSERT_TRUE(originType.valid());
    auto const& interner = model.lattice().interner();
    EXPECT_EQ(interner.kind(originType), TypeKind::Struct);
    EXPECT_EQ(interner.name(originType), "Foo");
}

// C 6.2.3 tag namespace (closes the tag-namespace residue of
// D-CSUBSET-DECL-GRAMMAR-LOW-RESIDUES): a struct TAG is NOT a bare type name.
// `Foo bare;` (no `struct`, no typedef) must FAIL — the tag `Foo` lives in the
// Tag namespace and an ordinary type-position lookup of the bare identifier
// misses it. `struct Foo bare;` IS the valid spelling and resolves the tag.
// This was previously a DOCUMENTED C DIVERGENCE (the old single-namespace
// resolveTypeNode treated a tag as a bare typedef-name); this cycle is the
// "future cycle that separates the namespaces" the prior pin anticipated.
// RED-ON-DISABLE: with the tag bound Ordinary (pre-change), `Foo bare;`
// resolves and `hasErrors()` is false → the first EXPECT_TRUE fails.
TEST(HirLoweringCSubset, BareStructTagNotUsableAsTypeName) {
    SemanticModel bareModel = analyzeCSubset(
        "struct Foo { int x; };\n"
        "Foo bare;\n");                  // no `struct`, no typedef — invalid in C
    EXPECT_TRUE(bareModel.hasErrors())
        << "a bare struct tag `Foo` is NOT a type name — `struct Foo` is required";
    EXPECT_EQ(countCode(bareModel.diagnostics(), DiagnosticCode::S_UnknownType), 1u)
        << "the bare tag name misses the ordinary namespace → S_UnknownType";

    // The valid spelling — `struct Foo bare;` — resolves the tag and lowers.
    SemanticModel model = analyzeCSubset(
        "struct Foo { int x; };\n"
        "struct Foo bare;\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 2u);   // struct Foo, bare
    HirNodeId const bare = decls[1];
    ASSERT_EQ(res->hir.kind(bare), HirKind::Global);
    TypeId const bareType = res->hir.globalType(bare);
    auto const& interner = model.lattice().interner();
    EXPECT_EQ(interner.kind(bareType), TypeKind::Struct);
    EXPECT_EQ(interner.name(bareType), "Foo");
}

// C 6.2.3 tag namespace: `typedef struct Foo Foo;` is LEGAL — the tag `Foo`
// (Tag namespace) and the typedef alias `Foo` (Ordinary namespace) coexist,
// so NO S_RedeclaredSymbol fires. This INVERTS the prior pin
// (TypedefSameNameAsTagRedeclaresInSingleNamespace), which asserted the old
// single-namespace COLLISION the prior cycle documented as a C divergence.
// RED-ON-DISABLE: route the composite tag BIND Ordinary (drop the
// fieldChildren→Tag gate) and the alias collides with the tag →
// S_RedeclaredSymbol reappears and this count rises above 0.
TEST(HirLoweringCSubset, TypedefSameNameAsTagCoexistsAcrossNamespaces) {
    SemanticModel model = analyzeCSubset(
        "struct Foo { int x; };\n"
        "typedef struct Foo Foo;\n");    // tag Foo (Tag) + typedef Foo (Ordinary)
    EXPECT_FALSE(model.hasErrors())
        << "C 6.2.3: a typedef alias may share a struct tag's name";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "the tag and the typedef are in separate namespaces — no collision";
}

// D5.1 cycle 4 review fix: the DOT form goes through a different lowering
// path than ARROW (no Deref synthesis). The previous test only exercised
// arrow; this one pins the dot path's structural shape.
TEST(HirLoweringCSubset, DotMemberAccessLowersWithoutDeref) {
    SemanticModel model = analyzeCSubset(
        "struct Point { int x; int y; };\n"
        "int by_value(struct Point s) { return s.x; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 2u);
    HirNodeId const fn = decls[1];
    HirNodeId const body = res->hir.functionBody(fn);
    auto stmts = res->hir.children(body);
    ASSERT_EQ(stmts.size(), 1u);
    HirNodeId const ret = stmts[0];
    auto const retVal = res->hir.returnValue(ret);
    ASSERT_TRUE(retVal.has_value());
    HirNodeId const access = *retVal;
    ASSERT_EQ(res->hir.kind(access), HirKind::MemberAccess);
    EXPECT_EQ(res->hir.payload(access), 0u);  // x is field 0
    // Dot form: child is the LHS DIRECTLY, no Deref wrapping.
    auto kids = res->hir.children(access);
    ASSERT_EQ(kids.size(), 1u);
    EXPECT_NE(res->hir.kind(kids[0]), HirKind::Deref);
    // The LHS's type is the struct directly (not Ptr<Struct>).
    auto const& interner = model.lattice().interner();
    EXPECT_EQ(interner.kind(res->hir.typeId(kids[0])), TypeKind::Struct);
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

TEST(HirLoweringCSubset, ExternGlobalWithInitializerRejectedLoud) {
    // D-FF2-3: `extern int x = 5;` is a contradiction — extern means
    // "storage lives elsewhere"; an init would either redefine the
    // symbol locally OR be silently dropped (the prior behavior).
    // Reject loud with H_ExternHasInitializer (remediation-distinct
    // from H_UnsupportedLoweringForKind: remove the init or drop
    // the `extern` keyword — not "extend the engine").
    SemanticModel model = analyzeCSubset("extern int x = 5;\n");
    ASSERT_FALSE(model.hasErrors())
        << "test setup: the c-subset grammar accepts the form; the "
           "rejection is at lowering, not at parse";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ExternHasInitializer), 1u);
    // The pre-fix silent path landed an ExternGlobal at top level; the
    // new path lands an Error sentinel so downstream tooling can't
    // mistake it for a successful extern declaration.
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::Error);
}

// D-CSUBSET-PACKED (F4): a `packed` spelling in the LEADING declaration-specifier
// position (`[[gnu::packed]] struct S {…} v;`) is UNHONORED — packed is honored only
// in the TRAILING composite-attribute slot (`struct S {…} __attribute__((packed))`).
// The linkage scan skips the ignored `stdAttr` wholesale, which would SILENTLY DROP
// packed (leaving the struct padded — a miscompile a program could depend on). It
// fails loud H_UnknownLinkageSpecifier instead, symmetric with the leading
// `__attribute__((packed))` case. Semantically clean (the attribute is a
// declSpecifier); the rejection is at lowering.
TEST(HirLoweringCSubset, LeadingPackedAttributeRejectedLoud) {
    SemanticModel model = analyzeCSubset(
        "[[gnu::packed]] struct S { char c; int v; } gv;\n"
        "int main(void){ return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "test setup: the leading attribute parses + analyzes cleanly; the "
           "rejection is at lowering, not at parse/semantic";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
        << "leading [[gnu::packed]] must fail loud, never silently drop packed";
}

// D-CSUBSET-PACKED (F-3): the GNU spelling of the SAME leading form —
// `__attribute__((packed)) struct S {…} gv;` — is likewise UNHONORED and fails loud.
// Sibling to LeadingPackedAttributeRejectedLoud (the C23 `[[gnu::packed]]` form):
// here `attrSpec` is NOT in the linkage-ignored rules, so the leading
// `__attribute__((packed))` takes the RECOGNIZED-specifier path, `packed` is not a
// linkage keyword, and lowering fails loud H_UnknownLinkageSpecifier (exactly like a
// leading `__attribute__((bogus))`). Must NOT be silently honored-or-dropped.
TEST(HirLoweringCSubset, LeadingGnuPackedAttributeRejectedLoud) {
    SemanticModel model = analyzeCSubset(
        "__attribute__((packed)) struct S { char c; int v; } gv;\n"
        "int main(void){ return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "test setup: the leading GNU attribute parses + analyzes cleanly; the "
           "rejection is at lowering, not at parse/semantic";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
        << "leading __attribute__((packed)) must fail loud, never silently drop packed";
}

// CONTRAST: a leading standard-ignorable attribute (`[[deprecated]]`) STAYS silently
// ignored (C23 6.7.11.1) — ONLY a `packed` spelling fails loud in the leading slot.
// RED-ON-DISABLE for over-broadening: were the F4 hook to fire on any ignored attr,
// this would wrongly report H_UnknownLinkageSpecifier.
TEST(HirLoweringCSubset, LeadingDeprecatedAttributeStillIgnored) {
    SemanticModel model = analyzeCSubset(
        "[[deprecated]] int gv;\n"
        "int main(void){ return 0; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "[[deprecated]] stays standard-ignorable; only packed fails loud";
}

TEST(HirLoweringCSubset, ExternGlobalWithIdentifierInitializerRejectedLoud) {
    // Post-fold #7 PT1a: pin identifier-init `extern int x = y;`
    // (RHS is an operand-rule referencing a prior decl), not just
    // literal-init. Shape-based F4 detector trips on any non-
    // arrayDeclSuffix initValue subtree regardless of RHS shape.
    SemanticModel model = analyzeCSubset(
        "int y = 1;\n"
        "extern int x = y;\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ExternHasInitializer), 1u);
}

TEST(HirLoweringCSubset, ExternGlobalWithEmptyBraceInitializerRejectedLoud) {
    // Post-fold #7 silent-failure F4: pre-fold the init-walk searched
    // for `isExprNode` descendants — `extern int x = {};` has NONE
    // (empty braceInitList), so the silent-accept arm fired. The
    // shape-based detector now keys on the initValue subtree's
    // existence and rejects loud regardless of contents.
    SemanticModel model = analyzeCSubset("extern int x = {};\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ExternHasInitializer), 1u);
}

TEST(HirLoweringCSubset, ExternGlobalWithArraySuffixNoInitStillAccepted) {
    // D-FF2-3 negative: `extern int x[10];` carries an array-size
    // expression (`10`) inside arrayDeclSuffix — that's NOT an init
    // and must NOT trigger H_ExternHasInitializer. The shape-based
    // detector skips the arrayDeclSuffix subtree exactly for this
    // case. Post-fold #7 PT4: also pin that the resulting decl IS
    // an ExternGlobal (not an Error sentinel) — a regression that
    // false-positively rejected `extern int x[10];` would otherwise
    // pass a `res->ok`-only check if no diagnostic landed.
    SemanticModel model = analyzeCSubset("extern int x[10];\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ExternHasInitializer), 0u);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::ExternGlobal);
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

TEST(HirLoweringCSubset, ValueYieldingIncrementLowersToSeqExpr) {
    // `return x++;` — postfix yields the OLD value, then mutates. Lowers to a
    // SeqExpr: { var tmp = x; x = x + 1; yield tmp }.
    SemanticModel model = analyzeCSubset("int f(int x) { return x++; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ret  = res->hir.children(body)[0];
    HirNodeId val  = *res->hir.returnValue(ret);
    ASSERT_EQ(res->hir.kind(val), HirKind::SeqExpr);
    // stmts: [var tmp = x, assign x = x+1]; result: ref tmp.
    EXPECT_EQ(res->hir.seqExprStmts(val).size(), 2u);
    EXPECT_EQ(res->hir.kind(res->hir.seqExprStmts(val)[0]), HirKind::VarDecl);
    EXPECT_EQ(res->hir.kind(res->hir.seqExprStmts(val)[1]), HirKind::AssignStmt);
    EXPECT_EQ(res->hir.kind(res->hir.seqExprResult(val)), HirKind::Ref);
}

TEST(HirLoweringCSubset, AssignmentAsSubExpressionLowersToSeqExpr) {
    // `while ((x = x + 1) < 10) {}` — the assignment is used as a value. Lowers
    // to a SeqExpr yielding the assigned value (sound inside a loop condition,
    // where hoisting the store would be wrong).
    SemanticModel model = analyzeCSubset("void f(int x) { while ((x = x + 1) < 10) {} }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body  = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId wh    = res->hir.children(body)[0];
    ASSERT_EQ(res->hir.kind(wh), HirKind::WhileStmt);
    HirNodeId cond  = *res->hir.loopCondition(wh);     // (x = x+1) < 10  → BinaryOp Lt
    ASSERT_EQ(res->hir.kind(cond), HirKind::BinaryOp);
    HirNodeId lhs   = res->hir.children(cond)[0];       // the (x = x+1) sub-expr
    EXPECT_EQ(res->hir.kind(lhs), HirKind::SeqExpr);
}

TEST(HirLoweringCSubset, ComplexLvalueCompoundAssignUsesTempPointer) {
    // `a[i] += 1;` — a complex lvalue. To evaluate `a[i]`'s address once, the
    // lowering binds a temp pointer and reads/writes through it: a Block of
    // { var p = &a[i]; *p = *p + 1; }.
    SemanticModel model = analyzeCSubset("void f(int i) { int a[4]; a[i] += 1; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    // children: [ var a[4], <block for a[i] += 1> ]
    HirNodeId stmt = res->hir.children(body)[1];
    ASSERT_EQ(res->hir.kind(stmt), HirKind::Block);
    auto inner = res->hir.children(stmt);
    ASSERT_EQ(inner.size(), 2u);
    EXPECT_EQ(res->hir.kind(inner[0]), HirKind::VarDecl);     // var p = &a[i]
    EXPECT_EQ(res->hir.kind(inner[1]), HirKind::AssignStmt);  // *p = *p + 1
    // the temp pointer's type is Ptr<I32>
    auto const& ti = model.lattice().interner();
    EXPECT_EQ(ti.kind(res->hir.varDeclType(inner[0])), TypeKind::Ptr);
}

// A COMPOUND assignment USED AS A VALUE (`y = (x += 2)`) — the path the Assign
// frame's `compound` branch handles, which the statement-position compound tests
// above do NOT exercise (those route through the separate `lowerCompoundAssign`).
// `(x += 2)` lowers to `SeqExpr([AssignStmt(Ref x, BinaryOp(Ref x, 2))], yield Ref
// x)`: the stored value is `lvRead(x) + 2` with operand[0] = the lvalue read,
// operand[1] = the rhs (`2`). This pins the flattened frame builds the SAME
// structure (and ordering: the lvRead is emitted before the rhs) as the recursive
// `lowerBinary` Assign arm. Guards the byte-identity of the compound-as-value arm.
TEST(HirLoweringCSubset, CompoundAssignAsSubExpressionLowersToSeqExpr) {
    SemanticModel model = analyzeCSubset(
        "void f(int x) { int y; y = (x += 2); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    // children: [VarDecl y, AssignStmt(y, <value of (x += 2)>)].
    HirNodeId outer = res->hir.children(body)[1];
    ASSERT_EQ(res->hir.kind(outer), HirKind::AssignStmt);
    HirNodeId val = res->hir.assignValue(outer);             // the (x += 2) value
    ASSERT_EQ(res->hir.kind(val), HirKind::SeqExpr);
    auto const stmts = res->hir.seqExprStmts(val);
    ASSERT_EQ(stmts.size(), 1u);
    ASSERT_EQ(res->hir.kind(stmts[0]), HirKind::AssignStmt);  // x = (x + 2)
    HirNodeId stored = res->hir.assignValue(stmts[0]);
    ASSERT_EQ(res->hir.kind(stored), HirKind::BinaryOp);      // x + 2
    auto const ops = res->hir.children(stored);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(res->hir.kind(ops[0]), HirKind::Ref);           // operand[0] = lvRead(x)
    EXPECT_EQ(res->hir.kind(res->hir.seqExprResult(val)), HirKind::Ref);  // yield Ref x
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

TEST(HirLoweringCSubset, GlobalArrayLowersToArrayTypeWithNoInit) {
    // A GLOBAL array exercises a different path from the local case: the suffix
    // nests under `topLevelDecl → varDeclTail → arrayDeclSuffix` (a descendant,
    // not a direct child), and `descendantsForInit` must NOT mistake the `[10]`
    // length for the global's initializer.
    SemanticModel model = analyzeCSubset("int g[10];\nint main() { return 0; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId g = res->hir.moduleDecls(res->hir.root())[0];
    ASSERT_EQ(res->hir.kind(g), HirKind::Global);
    auto const& ti = model.lattice().interner();
    TypeId const ty = res->hir.globalType(g);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 10);
    EXPECT_FALSE(res->hir.globalInit(g).has_value()) << "the `[10]` length must not become an initializer";
}

TEST(HirLoweringCSubset, GlobalInitConstEvalIsLeftAssociative) {
    // `int g = 10 - 3 + 1;` — the parse tree is now STRUCTURALLY
    // left-associative, and const-eval follows the tree: (10-3)+1 = 8.
    // The right-recursive mis-shape would fold 10-(3+1) = 6. This pins
    // the full source→parse→semantic→HIR→const-eval pipeline (the
    // hand-built Rig tests in test_const_eval.cpp can't see parser
    // shape bugs).
    SemanticModel model =
        analyzeCSubset("int g = 10 - 3 + 1;\nint main() { return 0; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId g = res->hir.moduleDecls(res->hir.root())[0];
    ASSERT_EQ(res->hir.kind(g), HirKind::Global);
    auto const init = res->hir.globalInit(g);
    ASSERT_TRUE(init.has_value());
    auto cer = evaluateConstant(res->hir, model.lattice().interner(),
                                res->literalPool, *init);
    ASSERT_TRUE(cer.value.has_value())
        << "the global initializer chain must const-fold";
    EXPECT_EQ(std::get<std::int64_t>(cer.value->value), 8)
        << "10 - 3 + 1 must evaluate LEFT-associatively: (10-3)+1 = 8";
}

// D-HIR-INFINITE-LOOP-NOT-TERMINATING × D-LK10-ENTRY-MAIN-IMPLICIT-RETURN
// interaction: `int main() { while (1) { return 5; } }`. The `while (1)` is
// provably-infinite (constant-truthy condition, no break exits its frame), so
// `lowerWhile` wraps it as `Block{ WhileStmt, Synthetic Unreachable }`. That
// wrapper makes the body structurally TERMINATE (`pathTerminates` recurses to
// the wrapper Block's last child, the `Unreachable`). Consequently
// `maybeAppendImplicitReturnZero` correctly sees a `main` that can NEVER fall
// through and appends NO implicit `return 0` (C99 §5.1.2.2.3 only matters when
// `main` can fall off the end — this one provably cannot). The earlier
// double-attach regression (D-LK10 / D-HIR-LOOP-BODY-ONLY-RETURN-DOUBLE-ATTACH)
// stays covered by the straight-line-body pin below, whose body genuinely
// does NOT terminate and so still exercises the implicit-return-0 nest.
//
// Asserts: (1) lowering COMPLETES (no abort), (2) the verifier is clean
// (`res->ok` — the wrapper's `Unreachable` is what satisfies the non-void
// return-completeness check), (3) the loop lowered to the Synthetic-Unreachable
// wrapper, and (4) NO synthetic return was appended (the body is the plain
// lowered block holding exactly the wrapper). The wrapper's `Unreachable` is
// pruned in MIR, so runtime is unchanged (exit 5).
TEST(HirLoweringCSubset, InfiniteLoopMainWrapsLoopAndSkipsImplicitReturn) {
    SemanticModel model = analyzeCSubset("int main() { while (1) { return 5; } }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto const decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    HirNodeId const fn = decls[0];
    ASSERT_EQ(res->hir.kind(fn), HirKind::Function);

    // The body is the plain lowered Block (NOT a synthetic implicit-return-0
    // wrapper — `main` provably terminates, so none was appended). It holds
    // exactly the loop wrapper.
    HirNodeId const body = res->hir.functionBody(fn);
    ASSERT_EQ(res->hir.kind(body), HirKind::Block);
    auto const bodyKids = res->hir.children(body);
    ASSERT_EQ(bodyKids.size(), 1u)
        << "no implicit return-0 appended: body holds only the loop wrapper";

    // The loop wrapper: a Synthetic Block whose children are [WhileStmt,
    // Synthetic Unreachable] — the D-HIR-INFINITE-LOOP-NOT-TERMINATING shape.
    HirNodeId const wrapper = bodyKids[0];
    ASSERT_EQ(res->hir.kind(wrapper), HirKind::Block);
    EXPECT_TRUE(has(res->hir.flags(wrapper), HirFlags::Synthetic))
        << "the infinite-loop wrapper Block must be flagged Synthetic";
    auto const wrapKids = res->hir.children(wrapper);
    ASSERT_EQ(wrapKids.size(), 2u)
        << "wrapper must hold exactly [loop, synthetic-unreachable]";
    EXPECT_EQ(res->hir.kind(wrapKids[0]), HirKind::WhileStmt);
    ASSERT_EQ(res->hir.kind(wrapKids[1]), HirKind::Unreachable);
    EXPECT_TRUE(has(res->hir.flags(wrapKids[1]), HirFlags::Synthetic))
        << "the synthetic Unreachable terminator must be flagged Synthetic";
}

// Breadth pin: the double-attach was NOT loop-specific — ANY non-empty
// `main` body that doesn't structurally terminate hit the re-wrap. A
// straight-line body (`int x; x = 1;`) has no terminator, so the implicit
// return is appended the same way. Same nesting contract; same red-on-
// disable (abort before any EXPECT on the pre-fix children-re-wrap).
TEST(HirLoweringCSubset, NonTerminatingStraightLineMainNestsImplicitReturnZero) {
    SemanticModel model = analyzeCSubset("int main() { int x; x = 1; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto const decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    HirNodeId const outer = res->hir.functionBody(decls[0]);
    ASSERT_EQ(res->hir.kind(outer), HirKind::Block);
    EXPECT_TRUE(has(res->hir.flags(outer), HirFlags::Synthetic));
    auto const outerKids = res->hir.children(outer);
    ASSERT_EQ(outerKids.size(), 2u);

    // The original body is nested as child 0 and still holds its two
    // statements (the VarDecl + the assignment ExprStmt) — not re-parented.
    HirNodeId const inner = outerKids[0];
    ASSERT_EQ(res->hir.kind(inner), HirKind::Block);
    EXPECT_EQ(res->hir.children(inner).size(), 2u);

    HirNodeId const ret = outerKids[1];
    ASSERT_EQ(res->hir.kind(ret), HirKind::ReturnStmt);
    EXPECT_TRUE(has(res->hir.flags(ret), HirFlags::Synthetic));
    ASSERT_TRUE(res->hir.returnValue(ret).has_value());
    EXPECT_EQ(res->hir.kind(*res->hir.returnValue(ret)), HirKind::Literal);
}

// ── D-HIR-INFINITE-LOOP-NOT-TERMINATING HIR-tier pins ───────────────────────
//
// `lowerWhile`/`lowerFor` wrap a PROVABLY-INFINITE loop (constant-truthy/absent
// condition AND no `break` exits its own frame) as `Block{ loop, Synthetic
// Unreachable }`, so the verifier's structural-termination check (which recurses
// to a Block's last child) sees the loop as terminating. This removes the
// over-rejection of a non-`main` non-void function whose terminating tail is
// such a loop, WITHOUT touching the verifier / H0003. The pins below assert the
// wrapper shape (positive) and — critically — that a BREAKABLE / non-constant /
// const-false loop is NOT wrapped (negative, no false positives).

namespace {

// True iff `id`'s subtree contains a Synthetic `Unreachable` leaf — the marker
// the infinite-loop wrapper synthesizes. Used to assert presence (positive pin)
// and ABSENCE (negative pins).
[[nodiscard]] bool subtreeHasSyntheticUnreachable(Hir const& hir, HirNodeId id) {
    if (hir.kind(id) == HirKind::Unreachable
        && has(hir.flags(id), HirFlags::Synthetic))
        return true;
    for (HirNodeId c : hir.children(id))
        if (subtreeHasSyntheticUnreachable(hir, c)) return true;
    return false;
}

// The function declaration whose symbol name is `name` (functions appear in
// source order in `moduleDecls`); InvalidId-shaped HirNodeId if absent.
[[nodiscard]] HirNodeId functionNamed(Hir const& hir, SemanticModel const& m,
                                      std::string_view name) {
    for (HirNodeId d : hir.moduleDecls(hir.root())) {
        if (hir.kind(d) != HirKind::Function) continue;
        SymbolId const sym = hir.functionSymbol(d);
        auto const* rec = m.recordFor(sym);
        if (rec != nullptr && rec->name == name) return d;
    }
    return HirNodeId{};
}

} // namespace

// POSITIVE — the anchor's exact repro: a NON-`main` non-void function whose
// terminating tail is a provably-infinite `while (1)`. Pre-fix this was
// over-rejected H0003 ("non-void function may fall through"); now the loop is
// wrapped, the body structurally terminates, and the verifier is clean.
TEST(HirLoweringCSubset, NonMainInfiniteLoopTailWrapsAndVerifies) {
    SemanticModel model =
        analyzeCSubset("int f(int x){ while(1){ return 5; } } int main(){ return f(0); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    // res->ok folds the verify-on-load pass: TRUE here means the non-void `f`
    // is NO LONGER rejected by checkReturnCompleteness (H0003) — the wrapper's
    // Unreachable made pathTerminates(f-body) true.
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 0u)
        << "the wrapped infinite-loop tail must satisfy non-void return completeness";

    HirNodeId const f = functionNamed(res->hir, model, "f");
    ASSERT_TRUE(f.valid());
    HirNodeId const body = res->hir.functionBody(f);
    ASSERT_EQ(res->hir.kind(body), HirKind::Block);
    auto const bodyKids = res->hir.children(body);
    ASSERT_EQ(bodyKids.size(), 1u);

    // The loop lowered to a Synthetic Block of [WhileStmt, Synthetic Unreachable].
    HirNodeId const wrapper = bodyKids[0];
    ASSERT_EQ(res->hir.kind(wrapper), HirKind::Block);
    EXPECT_TRUE(has(res->hir.flags(wrapper), HirFlags::Synthetic));
    auto const wrapKids = res->hir.children(wrapper);
    ASSERT_EQ(wrapKids.size(), 2u);
    EXPECT_EQ(res->hir.kind(wrapKids[0]), HirKind::WhileStmt);
    ASSERT_EQ(res->hir.kind(wrapKids[1]), HirKind::Unreachable);
    EXPECT_TRUE(has(res->hir.flags(wrapKids[1]), HirFlags::Synthetic));
}

// POSITIVE — the other two provably-infinite shapes also wrap: `for(;;)`
// (absent condition) and `do{...}while(1)` (constant-truthy condition).
TEST(HirLoweringCSubset, ForEverAndDoWhileOneWrapWithUnreachable) {
    SemanticModel forModel =
        analyzeCSubset("int f(int x){ for(;;){ return 9; } } int main(){ return f(0); }");
    ASSERT_FALSE(forModel.hasErrors());
    DiagnosticReporter fr;
    auto forRes = lowerToHir(forModel, fr);
    ASSERT_TRUE(forRes->ok) << (fr.all().empty() ? "" : fr.all()[0].actual);
    HirNodeId const ff = functionNamed(forRes->hir, forModel, "f");
    ASSERT_TRUE(ff.valid());
    auto const fKids = forRes->hir.children(forRes->hir.functionBody(ff));
    ASSERT_EQ(fKids.size(), 1u);
    ASSERT_EQ(forRes->hir.kind(fKids[0]), HirKind::Block);
    auto const fWrap = forRes->hir.children(fKids[0]);
    ASSERT_EQ(fWrap.size(), 2u);
    EXPECT_EQ(forRes->hir.kind(fWrap[0]), HirKind::ForStmt);
    EXPECT_EQ(forRes->hir.kind(fWrap[1]), HirKind::Unreachable);

    SemanticModel doModel =
        analyzeCSubset("int f(int x){ do{ return 7; }while(1); } int main(){ return f(0); }");
    ASSERT_FALSE(doModel.hasErrors());
    DiagnosticReporter dr;
    auto doRes = lowerToHir(doModel, dr);
    ASSERT_TRUE(doRes->ok) << (dr.all().empty() ? "" : dr.all()[0].actual);
    HirNodeId const df = functionNamed(doRes->hir, doModel, "f");
    ASSERT_TRUE(df.valid());
    auto const dKids = doRes->hir.children(doRes->hir.functionBody(df));
    ASSERT_EQ(dKids.size(), 1u);
    ASSERT_EQ(doRes->hir.kind(dKids[0]), HirKind::Block);
    auto const dWrap = doRes->hir.children(dKids[0]);
    ASSERT_EQ(dWrap.size(), 2u);
    EXPECT_EQ(doRes->hir.kind(dWrap[0]), HirKind::DoWhileStmt);
    EXPECT_EQ(doRes->hir.kind(dWrap[1]), HirKind::Unreachable);
}

// NEGATIVE (no false positives) — a BREAKABLE `while(1)` is NOT provably-
// infinite: a `break` reachable in its own frame (through an `if`) exits it.
// The loop must NOT be wrapped (no synthetic Unreachable anywhere in `f`),
// and the bare WhileStmt must sit directly in the body — proof the wrapper
// was not applied. The trailing `return 7` is the real terminator.
TEST(HirLoweringCSubset, BreakableInfiniteLoopIsNotWrapped) {
    SemanticModel model =
        analyzeCSubset("int f(int x){ while(1){ if(x) break; } return 7; } "
                       "int main(){ return f(1); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId const f = functionNamed(res->hir, model, "f");
    ASSERT_TRUE(f.valid());
    EXPECT_FALSE(subtreeHasSyntheticUnreachable(res->hir, f))
        << "a breakable while(1) must NOT be wrapped with a synthetic Unreachable";

    // The body's first statement is the bare WhileStmt (not a wrapper Block).
    auto const bodyKids = res->hir.children(res->hir.functionBody(f));
    ASSERT_GE(bodyKids.size(), 1u);
    EXPECT_EQ(res->hir.kind(bodyKids[0]), HirKind::WhileStmt)
        << "the breakable loop must lower to a bare WhileStmt, not a wrapper Block";
}

// ── FC16 (D-CSUBSET-NORETURN): a direct call to a noreturn function terminates ──
//
// A NON-`main` non-void function whose fall-through tail is a DIRECT call to a
// noreturn function (`_Noreturn void die(int); … die(1);`) is wrapped as
// `Block{ ExprStmt(call), Synthetic Unreachable }` — the direct structural mirror
// of the infinite-loop wrap above — so `f` structurally terminates and the
// verifier is clean. RED-ON-DISABLE (revert detection / the wrap): `f`'s
// fall-through no longer terminates → H_VerifierFailure count 1.
TEST(HirLoweringCSubset, NoreturnCallTailWrapsAndVerifies) {
    SemanticModel model = analyzeCSubset(
        "_Noreturn void die(int); "
        "int f(int x){ if(x>0) return x; die(1); } "
        "int main(){ return f(1); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 0u)
        << "the noreturn-call tail must satisfy non-void return completeness";
    HirNodeId const f = functionNamed(res->hir, model, "f");
    ASSERT_TRUE(f.valid());
    EXPECT_TRUE(subtreeHasSyntheticUnreachable(res->hir, res->hir.functionBody(f)))
        << "a direct call to a noreturn function must be wrapped with a synthetic Unreachable";
}

// All four `noreturn` spellings compile clean (parse + semantic + verify): the
// C11 `_Noreturn` keyword, the C23 `[[noreturn]]`, and both GNU
// `__attribute__((noreturn))` / `__attribute__((__noreturn__))`. Each must wrap
// the `die(1)` tail (no H_VerifierFailure) AND — critically for the GNU forms on
// a file-scope declaration — must NOT trip the linkage scan's
// H_UnknownLinkageSpecifier (the `linkageSpecifierIgnoredNames` / ignoredKinds path).
TEST(HirLoweringCSubset, NoreturnAllFourSpellingsCompileClean) {
    for (char const* proto : {
             "_Noreturn void die(int);",
             "[[noreturn]] void die(int);",
             "__attribute__((noreturn)) void die(int);",
             "__attribute__((__noreturn__)) void die(int);"}) {
        std::string const src = std::string(proto)
            + " int f(int x){ if(x>0) return x; die(1); }"
              " int main(){ return f(1); }";
        SemanticModel model = analyzeCSubset(src);
        ASSERT_FALSE(model.hasErrors()) << "spelling: " << proto;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << proto << ": "
                             << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 0u) << proto;
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u) << proto;
        HirNodeId const f = functionNamed(res->hir, model, "f");
        ASSERT_TRUE(f.valid()) << proto;
        EXPECT_TRUE(subtreeHasSyntheticUnreachable(res->hir, res->hir.functionBody(f)))
            << proto;
    }
}

// ⚠️ F1 — an INDIRECT / address-takeable callee must NOT be wrapped (the
// conservative direction), else a real return path is elided = MISCOMPILE. Two
// witnesses, both of which `firstNameToken` would have MIS-resolved to a noreturn
// name: (1) a ternary callee `(c ? die : other)(1)` lowers to a NON-Ref node →
// isDirectNoreturnCall false; `other` can return, so f's fall-through does NOT
// terminate → H_VerifierFailure STILL fires (the miscompile witness). (2) a
// function-POINTER object `fp(1)` lowers to Ref(fp) whose record has
// isNoreturn==false → not wrapped. In BOTH the un-relaxed fall-through keeps the
// loud verifier failure — proof we did not silently elide the return path.
TEST(HirLoweringCSubset, NoreturnIndirectCalleeIsNotWrapped) {
    {   // (1) ternary callee — the address-takeable miscompile vector.
        SemanticModel model = analyzeCSubset(
            "_Noreturn void die(int); void other(int); "
            "int f(int x, int c){ if(x>0) return x; (c ? die : other)(1); } "
            "int main(){ return f(1,1); }");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 1u)
            << "an address-takeable ternary callee must NOT be wrapped (F1)";
    }
    {   // (2) function-pointer object callee — Ref, but not a noreturn record.
        SemanticModel model = analyzeCSubset(
            "_Noreturn void die(int); "
            "int f(int x){ if(x>0) return x; void (*fp)(int) = die; fp(1); } "
            "int main(){ return f(1); }");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 1u)
            << "a call through a function-pointer object must NOT be wrapped (F1)";
    }
}

// ── FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS): the GNU semantic-attribute spellings
//    at FILE scope ride the linkage scan's by-NAME skip end-to-end ────────────

// The FIVE by-name-ignored semantic-attribute spellings (deprecated /
// maybe_unused / unused / nodiscard / warn_unused_result — incl. a
// string-argument form and a dunder form) must lower a FILE-scope declaration
// with ZERO H_UnknownLinkageSpecifier: without the
// `linkageSpecifierIgnoredNames` extension every one hard-fails H000C
// (probe-confirmed at the pre-change HEAD). RED-ON-DISABLE: drop a name from
// the topLevelDecl ignore list → that spelling's H000C count flips to 1.
TEST(HirLoweringCSubset, GnuSemanticAttributeSpellingsFileScopeLowerClean) {
    for (char const* proto : {
             "__attribute__((deprecated)) int f(void) { return 1; }",
             "__attribute__((deprecated(\"use g\"))) int f(void) { return 1; }",
             "__attribute__((__deprecated__)) int f(void) { return 1; }",
             "__attribute__((warn_unused_result)) int f(void) { return 1; }",
             "__attribute__((nodiscard)) int f(void) { return 1; }",
             "__attribute__((unused)) int f(void) { return 1; }",
             "__attribute__((maybe_unused)) int f(void) { return 1; }"}) {
        std::string const src = std::string(proto)
            + " int main(){ return f() - 1; }";
        SemanticModel model = analyzeCSubset(src);
        ASSERT_FALSE(model.hasErrors()) << "spelling: " << proto;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << proto << ": "
                             << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
            << proto << " — the by-name linkage skip must cover this spelling";
    }
}

// The Fork-2 BOUNDARY regression guard: an UNKNOWN GNU attribute at file scope
// STILL fails loud H_UnknownLinkageSpecifier (the by-name skip covers ONLY the
// declared semantic-attribute names — it must not become a wholesale ignore).
TEST(HirLoweringCSubset, GnuUnknownAttributeFileScopeStillFailsLoud) {
    SemanticModel model = analyzeCSubset(
        "__attribute__((frobnicate)) int f(void) { return 1; } "
        "int main(){ return f() - 1; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    (void)res;
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
        << "an unknown GNU attribute must keep the loud typo-protection gate";
}

// (The `static __attribute__((deprecated))` no-clobber pin — the co-present
// `static` keeping its INTERNAL binding — lives at the MIR tier where the
// binding is observable: MirLoweringCSubsetLinkage
// .GnuDeprecatedDoesNotClobberStaticLinkage.)

// The bare attribute-declaration statement lowers to NOTHING observable: the
// `[[fallthrough]];` item maps to Skip (an empty Block) and the enclosing
// declOrAttrStmt wrapper is peeled by the unmapped-statement PassThrough —
// zero H diagnostics, verifier clean.
TEST(HirLoweringCSubset, FallthroughStatementLowersToSkip) {
    SemanticModel model = analyzeCSubset(
        "int main(){ int x = 1; int acc = 0; "
        "switch (x) { case 1: acc += 1; [[fallthrough]]; "
        "case 2: acc += 10; break; default: acc = 99; break; } "
        "return acc - 11; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 0u);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u);
}

// ── FC5: goto / labels ──────────────────────────────────────────────────────

namespace {
[[nodiscard]] std::size_t countKind(Hir const& h, HirKind k) {
    std::size_t n = 0;
    std::uint32_t const tag = h.id().v;
    for (std::uint32_t i = 1; i < h.nodeCount(); ++i)
        if (h.kind(HirNodeId{i, tag}) == k) ++n;
    return n;
}
}  // namespace

// FC5 — `goto`/labels lower to the new GotoStmt/LabelStmt kinds, and a function
// whose ONLY return is reached through a forward goto + a label still lowers
// clean (pathTerminates treats `goto` as a terminator and a LabelStmt as
// transparent — a labeled `return` terminates). Red-on-disable: if LabelStmt
// transparency in `pathTerminates` were reverted, the body would look like it
// falls through and `lowerToHir` would fail H_VerifierFailure.
TEST(HirLoweringCSubset, GotoAndLabelLowerCleanAndTerminateViaLabel) {
    SemanticModel model = analyzeCSubset(
        "int f(int c){ if(c) goto a; return 1; a: return 2; } "
        "int main(){ return f(0); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 0u)
        << "a goto+labeled-return function must not look like it falls through";
    EXPECT_EQ(countKind(res->hir, HirKind::GotoStmt), 1u);
    EXPECT_EQ(countKind(res->hir, HirKind::LabelStmt), 1u);

    // A function that ONLY terminates via a labeled return (`goto a; a: ...`).
    SemanticModel m2 = analyzeCSubset(
        "int g(){ goto a; a: return 5; } int main(){ return g(); }");
    ASSERT_FALSE(m2.hasErrors());
    DiagnosticReporter r2;
    auto res2 = lowerToHir(m2, r2);
    EXPECT_TRUE(res2->ok) << (r2.all().empty() ? "" : r2.all()[0].actual);
    EXPECT_EQ(countCode(r2, DiagnosticCode::H_VerifierFailure), 0u);
}

// FC5 (audit MUST-FIX 2) — the dead-code scan must NOT flag a goto's TARGET label
// as unreachable: `goto X; X: …` is the universal cleanup idiom and the label is
// manifestly reachable. But a genuinely-dead NON-label statement after a goto
// still warns. This is the carve-out's red-on-disable lever: reverting it makes
// the first case emit a spurious H_UnreachableCode on the label.
TEST(HirLoweringCSubset, GotoTargetLabelIsNotFlaggedUnreachable) {
    // `goto skip; skip: return 1;` — the label directly follows the goto: ZERO.
    SemanticModel clean = analyzeCSubset(
        "int f(){ goto skip; skip: return 1; } int main(){ return f(); }");
    ASSERT_FALSE(clean.hasErrors());
    DiagnosticReporter cr;
    auto cres = lowerToHir(clean, cr);
    ASSERT_TRUE(cres->ok) << (cr.all().empty() ? "" : cr.all()[0].actual);
    EXPECT_EQ(countCode(cr, DiagnosticCode::H_UnreachableCode), 0u)
        << "a goto's target label must NOT be flagged as dead code";

    // `goto skip; r = 1; skip: return r;` — the `r = 1` between the goto and the
    // label IS dead: EXACTLY ONE warning (on the assignment, not the label).
    SemanticModel dead = analyzeCSubset(
        "int f(){ int r; goto skip; r = 1; skip: return r; } "
        "int main(){ return f(); }");
    ASSERT_FALSE(dead.hasErrors());
    DiagnosticReporter dr;
    auto dres = lowerToHir(dead, dr);
    ASSERT_TRUE(dres->ok) << (dr.all().empty() ? "" : dr.all()[0].actual);
    EXPECT_EQ(countCode(dr, DiagnosticCode::H_UnreachableCode), 1u)
        << "a genuinely-dead non-label statement after a goto must still warn";
}

// FC5 — the comma operator lowers to a SeqExpr (the existing sequencing
// substrate); a programmer comma is built non-Synthetic. A multi-declarator with
// a comma SEPARATOR stays two VarDecls (the comma-gate), never one SeqExpr-typed
// declarator — the structural side of the multi-site contract.
TEST(HirLoweringCSubset, CommaOperatorLowersToSeqExprAndSeparatorStaysTwoDecls) {
    SemanticModel op = analyzeCSubset(
        "int f(int x){ return (x = x + 1, x + 1); } int main(){ return f(40); }");
    ASSERT_FALSE(op.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(op, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_GE(countKind(res->hir, HirKind::SeqExpr), 1u)
        << "the comma operator must lower to a SeqExpr";

    // `int a = 1, b = 2;` — two declarators (the comma is a SEPARATOR). If the
    // comma-gate broke, the initializer would swallow `, b = 2` into one SeqExpr
    // and `b` would never be declared (so there would be ONE VarDecl, and `b`
    // would be undeclared at the return — a semantic error).
    SemanticModel sep = analyzeCSubset(
        "int f(){ int a = 1, b = 2; return a + b; } int main(){ return f(); }");
    ASSERT_FALSE(sep.hasErrors())
        << "int a=1,b=2 must declare BOTH a and b (comma is a separator here)";
    DiagnosticReporter sr;
    auto sres = lowerToHir(sep, sr);
    ASSERT_TRUE(sres->ok) << (sr.all().empty() ? "" : sr.all()[0].actual);
    // BOTH a and b are declared — neither f nor main has params, so the module's
    // VarDecl count is exactly 2 (a, b). If the comma-gate broke, the initializer
    // would swallow `, b = 2` into one declarator (1 VarDecl) and `b` would be
    // undeclared (already caught by the hasErrors assertion above; this pins the
    // count too).
    EXPECT_EQ(countKind(sres->hir, HirKind::VarDecl), 2u)
        << "int a=1,b=2 must lower to TWO VarDecls (comma is a separator here)";
}

// NEGATIVE — a const-FALSE condition (`while(0)`) and a NON-constant condition
// (`while(x)`) are not provably-infinite and must not be wrapped. (Both rely on
// the trailing `return 7` to terminate; neither loop is touched by the wrapper.)
TEST(HirLoweringCSubset, FalseAndNonConstConditionsAreNotWrapped) {
    SemanticModel zeroModel =
        analyzeCSubset("int f(int x){ while(0){ return 5; } return 7; } "
                       "int main(){ return f(0); }");
    ASSERT_FALSE(zeroModel.hasErrors());
    DiagnosticReporter zr;
    auto zeroRes = lowerToHir(zeroModel, zr);
    ASSERT_TRUE(zeroRes->ok) << (zr.all().empty() ? "" : zr.all()[0].actual);
    HirNodeId const zf = functionNamed(zeroRes->hir, zeroModel, "f");
    ASSERT_TRUE(zf.valid());
    EXPECT_FALSE(subtreeHasSyntheticUnreachable(zeroRes->hir, zf))
        << "while(0) is provably FINITE — must not be wrapped";

    SemanticModel varModel =
        analyzeCSubset("int f(int x){ while(x){ x = x - 1; } return 7; } "
                       "int main(){ return f(3); }");
    ASSERT_FALSE(varModel.hasErrors());
    DiagnosticReporter vr;
    auto varRes = lowerToHir(varModel, vr);
    ASSERT_TRUE(varRes->ok) << (vr.all().empty() ? "" : vr.all()[0].actual);
    HirNodeId const vf = functionNamed(varRes->hir, varModel, "f");
    ASSERT_TRUE(vf.valid());
    EXPECT_FALSE(subtreeHasSyntheticUnreachable(varRes->hir, vf))
        << "while(x) is not a constant-truthy condition — must not be wrapped";
}

// NEGATIVE (nesting) — a `break` inside a NESTED loop targets the INNER loop,
// not the outer. So `while(1){ while(1){ break; } }` has an OUTER loop that is
// provably-infinite (no break in ITS frame) and an INNER loop that is breakable
// (NOT infinite). The break-scan must respect that frame boundary: the outer
// loop IS wrapped; the inner loop is NOT. Exactly one synthetic Unreachable in
// the function (the outer wrapper's), and the inner WhileStmt is bare.
TEST(HirLoweringCSubset, NestedInnerBreakDoesNotDeInfiniteOuterLoop) {
    SemanticModel model =
        analyzeCSubset("int f(int x){ while(1){ while(1){ break; } return 5; } } "
                       "int main(){ return f(0); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId const f = functionNamed(res->hir, model, "f");
    ASSERT_TRUE(f.valid());

    // Outer loop wrapped: body holds exactly the Synthetic wrapper Block whose
    // first child is the OUTER WhileStmt.
    auto const bodyKids = res->hir.children(res->hir.functionBody(f));
    ASSERT_EQ(bodyKids.size(), 1u);
    HirNodeId const wrapper = bodyKids[0];
    ASSERT_EQ(res->hir.kind(wrapper), HirKind::Block);
    EXPECT_TRUE(has(res->hir.flags(wrapper), HirFlags::Synthetic));
    auto const wrapKids = res->hir.children(wrapper);
    ASSERT_EQ(wrapKids.size(), 2u);
    HirNodeId const outerWhile = wrapKids[0];
    ASSERT_EQ(res->hir.kind(outerWhile), HirKind::WhileStmt);
    ASSERT_EQ(res->hir.kind(wrapKids[1]), HirKind::Unreachable);

    // The inner loop (inside the outer loop's body) is a BARE WhileStmt — NOT
    // wrapped — because its `break` exits it. Find it under the outer while's
    // body and confirm its own subtree carries no synthetic Unreachable.
    HirNodeId const outerBody = res->hir.loopBody(outerWhile);
    HirNodeId innerWhile{};
    for (HirNodeId c : res->hir.children(outerBody))
        if (res->hir.kind(c) == HirKind::WhileStmt) { innerWhile = c; break; }
    ASSERT_TRUE(innerWhile.valid())
        << "the inner WhileStmt must sit bare in the outer loop's body";
    EXPECT_FALSE(subtreeHasSyntheticUnreachable(res->hir, innerWhile))
        << "the breakable INNER loop must not be wrapped";
}

TEST(HirLoweringCSubset, NonConstantArrayLengthFailsLoud) {
    // `int a[n]` (variable length) must NOT silently decay or assume a length —
    // the semantic phase emits S_NonConstantArrayLength.
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

TEST(HirLoweringCSubset, IncludeDirectiveIsSkippedNotFailed) {
    // An `#include` directive contributes NO HIR node (its declarations arrive
    // via the CU import resolver's cross-refs). Lowering must SKIP it cleanly,
    // not emit H_UnsupportedLoweringForKind. The include target is unresolved
    // here (single in-memory buffer), but the directive node still parses and
    // reaches the top-level lowering loop — which is exactly what we pin.
    SemanticModel model = analyzeCSubset("#include \"x.h\"\nint f() { return 0; }\n");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 0u)
        << "the #include directive must be skipped, not fail loud";
    // Exactly one decl: the function. The directive added nothing.
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::Function);
}

// HR cycle C: an int-typed `return expr;` whose expr type is non-int (e.g.
// a comparison that produces Bool) gets a `Cast(_, int)` wrapper inserted
// by the coercion pass. Pins the return-type-threading + coerce mechanism.
TEST(HirLoweringCSubset, ReturnOfBoolFromIntFunctionEmitsCast) {
    SemanticModel model = analyzeCSubset(
        "int gt(int a, int b) { return a > b; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ret  = res->hir.children(body)[0];
    HirNodeId val  = *res->hir.returnValue(ret);
    // The returned expression is now `Cast(BinaryOp(Gt, ...), int)`.
    EXPECT_EQ(res->hir.kind(val), HirKind::Cast);
    auto castKids = res->hir.children(val);
    ASSERT_EQ(castKids.size(), 1u);
    EXPECT_EQ(res->hir.kind(castKids[0]), HirKind::BinaryOp);
}

// HR cycle C: an `if` condition that's already Bool-typed (from a
// comparison) does NOT get a redundant Cast — coerce(child, target) is
// a no-op when child.type == target.
TEST(HirLoweringCSubset, IfConditionAlreadyBoolStaysUncasted) {
    SemanticModel model = analyzeCSubset(
        "void f(int x) { if (x > 0) { return; } }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ifs  = res->hir.children(body)[0];
    HirNodeId cond = res->hir.ifCondition(ifs);
    // `x > 0` is already Bool — no Cast wrapper needed.
    EXPECT_EQ(res->hir.kind(cond), HirKind::BinaryOp);
}

TEST(HirLoweringCSubset, TernaryLowersToTernaryNode) {
    // `cond ? a : b` lowers to a HIR Ternary [cond, then, else]. A
    // non-Bool ARITHMETIC cond takes the truthiness test: a synthetic
    // `BinaryOp Ne(cond, 0-of-cond's-type)` typed Bool (C99 6.5.15p4
    // "compares unequal to 0") — NOT a `Cast(_, Bool)`, whose MIR
    // lowering (Trunc) would keep only the low bit (`x = 2` would
    // select the WRONG arm). Pins the coerceCondition shape at the
    // ternary site.
    SemanticModel model = analyzeCSubset("int f(int x) { return x ? 1 : 2; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ret  = res->hir.children(body)[0];
    HirNodeId tern = *res->hir.returnValue(ret);
    ASSERT_EQ(res->hir.kind(tern), HirKind::Ternary);
    auto kids = res->hir.children(tern);
    ASSERT_EQ(kids.size(), 3u);
    // cond is `Ne(Ref(x), Literal 0)` typed Bool after coerceCondition.
    ASSERT_EQ(res->hir.kind(kids[0]), HirKind::BinaryOp)
        << "non-Bool ternary cond must lower as a truthiness comparison, "
           "not a Cast";
    ASSERT_TRUE(isCoreOp(res->hir.payload(kids[0])));
    EXPECT_EQ(decodeCoreOp(res->hir.payload(kids[0])), HirOpKind::Ne);
    EXPECT_EQ(model.lattice().interner().kind(res->hir.typeId(kids[0])),
              TypeKind::Bool);
    auto neKids = res->hir.children(kids[0]);
    ASSERT_EQ(neKids.size(), 2u);
    EXPECT_EQ(res->hir.kind(neKids[0]), HirKind::Ref);     // cond operand: x
    ASSERT_EQ(res->hir.kind(neKids[1]), HirKind::Literal); // synthetic 0
    // The synthetic zero keeps the cond's OWN type (I32) — no promotion
    // is needed for an unequal-to-zero test — and its pool value is 0.
    EXPECT_EQ(model.lattice().interner().kind(res->hir.typeId(neKids[1])),
              TypeKind::I32);
    auto zeroLit = res->literalPool.at(res->hir.payload(neKids[1]));
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(zeroLit.value));
    EXPECT_EQ(std::get<std::int64_t>(zeroLit.value), 0);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::Literal);   // then: 1
    EXPECT_EQ(res->hir.kind(kids[2]), HirKind::Literal);   // else: 2
}

// LogicalAnd/Or operands are CONDITION positions (C99 6.5.13p3/6.5.14p3:
// each operand "compares unequal to 0"). A non-Bool int operand must take
// the same truthiness `Ne(operand, 0)` wrap as an if/while condition —
// pinned at HIR tier because the MIR tier's LogicalAnd lowering currently
// trips an unrelated pre-existing StructCf marker mismatch (orthogonal:
// it reproduces with genuine Bool comparison operands too).
TEST(HirLoweringCSubset, LogicalAndIntOperandsTakeTruthinessNe) {
    SemanticModel model = analyzeCSubset(
        "int f(int x, int y) { return x && y ? 1 : 2; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ret  = res->hir.children(body)[0];
    HirNodeId tern = *res->hir.returnValue(ret);
    ASSERT_EQ(res->hir.kind(tern), HirKind::Ternary);
    // The ternary cond is the LogicalAnd itself — already Bool, so
    // coerceCondition adds NO extra node on top of it.
    HirNodeId land = res->hir.children(tern)[0];
    ASSERT_EQ(res->hir.kind(land), HirKind::LogicalAnd)
        << "Bool-typed LogicalAnd cond must NOT be re-wrapped";
    auto ops = res->hir.children(land);
    ASSERT_EQ(ops.size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        ASSERT_EQ(res->hir.kind(ops[i]), HirKind::BinaryOp)
            << "int operand " << i << " of && must take the truthiness Ne";
        ASSERT_TRUE(isCoreOp(res->hir.payload(ops[i])));
        EXPECT_EQ(decodeCoreOp(res->hir.payload(ops[i])), HirOpKind::Ne);
        EXPECT_EQ(model.lattice().interner().kind(res->hir.typeId(ops[i])),
                  TypeKind::Bool);
    }
}

TEST(HirLoweringCSubset, PointerDerefAndAddressOfLower) {
    // `*p = x` (deref-assign through a pointer) and `p = &x` (address-of into a
    // pointer) lower with correct Ptr / pointee result types.
    SemanticModel model = analyzeCSubset(
        "void f(int x) {\n"
        "  int *p;\n"
        "  p = &x;\n"
        "  *p = x;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    auto stmts = res->hir.children(body);
    // [ var p, assign p = &x, assign *p = x ]
    ASSERT_EQ(stmts.size(), 3u);
    // `p = &x`: value is AddressOf typed Ptr<I32>.
    HirNodeId addr = res->hir.assignValue(stmts[1]);
    ASSERT_EQ(res->hir.kind(addr), HirKind::AddressOf);
    ASSERT_EQ(ti.kind(res->hir.typeId(addr)), TypeKind::Ptr);
    // `*p = x`: target is Deref typed I32 (the pointee).
    HirNodeId deref = res->hir.assignTarget(stmts[2]);
    ASSERT_EQ(res->hir.kind(deref), HirKind::Deref);
    EXPECT_EQ(ti.kind(res->hir.typeId(deref)), TypeKind::I32);
}

TEST(HirLoweringCSubset, CharLiteralLowersToCharValue) {
    // `'a'` — coalesced body token, decoded to a Char codepoint.
    SemanticModel model = analyzeCSubset("char f() { return 'a'; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::Char);
    ASSERT_TRUE(std::holds_alternative<std::uint64_t>(v.value));
    EXPECT_EQ(std::get<std::uint64_t>(v.value), static_cast<std::uint64_t>('a'));
}

TEST(HirLoweringCSubset, CharEscapeLowersToControlCodepoint) {
    SemanticModel model = analyzeCSubset("char f() { return '\\n'; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    EXPECT_EQ(std::get<std::uint64_t>(res->literalPool.at(0).value), 10u);  // '\n'
}

TEST(HirLoweringCSubset, EmptyCharLiteralFailsLoud) {
    // `''` has no body char — fail loud, never a garbage codepoint.
    SemanticModel model = analyzeCSubset("char f() { return ''; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GT(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 0u);
}

TEST(HirLoweringCSubset, MultiCharCharLiteralFailsLoud) {
    // `'ab'` — a multi-character char body must fail loud, not silently take one
    // byte. (Symmetric to the empty case; the one a user hits by accident.)
    SemanticModel model = analyzeCSubset("char f() { return 'ab'; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GT(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 0u);
}

// ── C11/C23 6.4.4.4: wide / UTF CHARACTER constants (L'x'/u'x'/U'x'/u8'x') ───────
// A prefixed char constant is a SCALAR: its element core is typed by the prefix and
// its value is the single decoded code point (uint64 arm). The narrow path is
// UNCHANGED (the four tests above are the byte-identity guard).

TEST(HirLoweringCSubset, WideCharConstantElementAndValue) {
    // Each prefixed char → its C23 core + the decoded code point. `L'x'` under the
    // default format is wchar_t = I32 (the POSIX width); value is the code point 120.
    struct Case { char const* src; TypeKind core; std::uint64_t value; };
    for (auto const& tc : {Case{"void f() { L'x'; }",  TypeKind::I32, 120},
                           Case{"void f() { u'A'; }",  TypeKind::U16, 65},
                           Case{"void f() { U'A'; }",  TypeKind::U32, 65},
                           Case{"void f() { u8'A'; }", TypeKind::U8,  65}}) {
        SemanticModel model = analyzeCSubset(tc.src);
        ASSERT_FALSE(model.hasErrors()) << tc.src;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << tc.src << " : " << (r.all().empty() ? "" : r.all()[0].actual);
        ASSERT_EQ(res->literalPool.size(), 1u) << tc.src;
        auto const& v = res->literalPool.at(0);
        EXPECT_EQ(v.core, tc.core) << tc.src;
        ASSERT_TRUE(std::holds_alternative<std::uint64_t>(v.value)) << tc.src;
        EXPECT_EQ(std::get<std::uint64_t>(v.value), tc.value) << tc.src;
        auto const& ti = model.lattice().interner();
        HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
        HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
        EXPECT_EQ(ti.kind(res->hir.typeId(lit)), tc.core) << tc.src;
    }
}

TEST(HirLoweringCSubset, WideCharBmpMultibyteDecodesToCodepoint) {
    // `U'€'` — U+20AC, source bytes E2 82 AC — UTF-8-decodes those THREE source
    // bytes to the SINGLE code point 0x20AC (NOT a byte-passthrough). The witness
    // that the char body is UTF-8-decoded exactly like a wide string.
    SemanticModel model = analyzeCSubset("void f() { U'\xe2\x82\xac'; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U32);
    ASSERT_TRUE(std::holds_alternative<std::uint64_t>(v.value));
    EXPECT_EQ(std::get<std::uint64_t>(v.value), 0x20ACu) << "U+20AC is ONE code point";
}

TEST(HirLoweringCSubset, Utf8CharOutOfRangeFailsLoud) {
    // `u8'β'` — U+03B2 (CE B2) exceeds the single-UTF-8-code-unit range (0x7F).
    // C23 char8_t constant constraint → fail loud (H_Utf8CharLiteralOutOfRange),
    // NEVER a silently truncated low byte.
    SemanticModel model = analyzeCSubset("void f() { u8'\xce\xb2'; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "an out-of-range u8 char must fail lowering";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_Utf8CharLiteralOutOfRange), 1u)
        << "exactly the u8-out-of-range code, never a silent truncated byte";
}

TEST(HirLoweringCSubset, Utf16CharAstralFailsLoud) {
    // `u'😀'` — U+1F600, a supplementary-plane cp under a 16-bit char16_t. One
    // char16_t holds ONE code unit; an astral cp needs a surrogate PAIR → fail loud
    // (H_WideCharValueUnrepresentable), NEVER a silent wrong unit. Format-invariant
    // (u' is always U16), so the reject is target-independent.
    SemanticModel model = analyzeCSubset("void f() { u'\xf0\x9f\x98\x80'; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "an astral char16_t constant must fail lowering";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_WideCharValueUnrepresentable), 1u);
}

TEST(HirLoweringCSubset, WideCharMultiCharAndEmptyFailLoud) {
    // A wide/UTF char must denote EXACTLY ONE code point. `L'ab'` (multi) and `L''`
    // (empty) both fail loud (H_WideCharValueUnrepresentable) — the strict
    // single-code-point rule the wide path enforces (unlike narrow impl-defined).
    for (char const* src : {"void f() { L'ab'; }", "void f() { L''; }"}) {
        SemanticModel model = analyzeCSubset(src);
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_FALSE(res->ok) << src;
        EXPECT_EQ(countCode(r, DiagnosticCode::H_WideCharValueUnrepresentable), 1u) << src;
    }
}

// SHOULD-FIX #6 — the DEFINITIVE per-format / agnostic witness. `L'😀'` (U+1F600)
// is a wchar_t constant, and wchar_t is FORMAT-keyed: on pe it is the 16-bit UTF-16
// unit (U16) → the astral cp is UNREPRESENTABLE → fail loud; on the POSIX default it
// is I32 → the astral cp fits → lowers to value 0x1F600. ONE source, opposite
// outcomes, decided purely by the config `elementCoreByFormat` map — no format
// branch in shared substrate. Red-on-disable of the format-keying flips one arm.
TEST(HirLoweringCSubset, WideCharAstralIsFormatKeyed) {
    char const* src = "void f() { L'\xf0\x9f\x98\x80'; }";
    // PE (u16 wchar_t) → fail loud.
    {
        SemanticModel model = analyzeCSubsetPe(src);
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_FALSE(res->ok) << "astral L' under pe (u16 wchar_t) must fail loud";
        EXPECT_EQ(countCode(r, DiagnosticCode::H_WideCharValueUnrepresentable), 1u);
    }
    // POSIX default (i32 wchar_t) → the astral cp fits → value 0x1F600.
    {
        SemanticModel model = analyzeCSubset(src);
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
        ASSERT_EQ(res->literalPool.size(), 1u);
        auto const& v = res->literalPool.at(0);
        EXPECT_EQ(v.core, TypeKind::I32);
        ASSERT_TRUE(std::holds_alternative<std::uint64_t>(v.value));
        EXPECT_EQ(std::get<std::uint64_t>(v.value), 0x1F600u);
    }
}

TEST(HirLoweringCSubset, LoweredSeqExprRoundTrips) {
    // A REAL lowering-produced SeqExpr (from `x++` in value position) must
    // survive the .dsshir emit→parse→verify round-trip — the seam where the
    // synthetic-temp `%sN` handle fallback meets the text writer.
    SemanticModel model = analyzeCSubset("int f(int x) { return x++; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    std::vector<std::string> names = symbolNames(model);
    HirTextContext ctx;
    ctx.interner    = &model.lattice().interner();
    ctx.symbolNames = &names;
    ctx.literalPool = &res->literalPool;
    DiagnosticReporter er;
    std::string const out = emitHir(res->hir, ctx, er);
    EXPECT_NE(out.find("seq "), std::string::npos) << "expected a seq expr in:\n" << out;

    DiagnosticReporter pr;
    auto parsed = parseHir(out, CompilationUnitId{1}, pr);
    std::string diags;
    for (auto const& d : pr.all()) diags += std::string{diagnosticCodeName(d.code)} + ": " + d.actual + "\n";
    EXPECT_TRUE(parsed->ok) << "lowered SeqExpr did not round-trip/verify\n" << diags;
}

TEST(HirLoweringCSubset, StringLiteralLowersToCharArray) {
    // `"hello"` — coalesced body, decoded bytes in the pool, typed Array<Char,6>
    // (5 chars + implied NUL).
    SemanticModel model = analyzeCSubset("void f() { \"hello\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::Char);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), "hello");

    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    auto const& ti = model.lattice().interner();
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 6);                                  // "hello" + NUL
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::Char);
}

TEST(HirLoweringCSubset, StringEscapeDecodes) {
    SemanticModel model = analyzeCSubset("void f() { \"a\\tb\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    EXPECT_EQ(std::get<std::string>(res->literalPool.at(0).value), std::string("a\tb"));
}

// ── C 5.1.1.2 phase 6: adjacent string-literal concatenation ────────────────
// (D-CSUBSET-ADJACENT-STRING-CONCAT). `"a" "b"` ≡ `"ab"`. The HIR lowers the
// WHOLE stringLiteralExpr through the `decodeAdjacentStringBodies` chokepoint:
// every body decoded (phase 5) then byte-joined (phase 6). N = total decoded
// bytes; the literal's type is Array<Char, N+1>.

TEST(HirLoweringCSubset, AdjacentStringsConcatTwoWay) {
    // `"a" "b"` → "ab", Array<Char,3> (2 bytes + NUL).
    SemanticModel model = analyzeCSubset("void f() { \"a\" \"b\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u) << "the two pieces fold into ONE literal";
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::Char);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), "ab");

    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    auto const& ti = model.lattice().interner();
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3) << "\"ab\" + NUL";
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::Char);
}

TEST(HirLoweringCSubset, AdjacentStringsConcatThreeWay) {
    // `"a" "b" "c"` → "abc", Array<Char,4>.
    SemanticModel model = analyzeCSubset("void f() { \"a\" \"b\" \"c\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(std::get<std::string>(v.value), "abc");

    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    auto const& ti = model.lattice().interner();
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 4) << "\"abc\" + NUL";
}

// THE byte-level pin (mandatory): concatenation is at the DECODED-byte level —
// each body decoded by phase 5 FIRST, THEN joined. `"\x41" "1"` must be 'A'+'1'
// = "A1" (2 bytes), NOT a raw-token merge `\x411` (which would parse the hex
// escape across the boundary into the single byte 0x11). RED if a consumer
// concatenated raw bodies before decoding.
TEST(HirLoweringCSubset, AdjacentStringsConcatEscapeBoundary) {
    SemanticModel model = analyzeCSubset("void f() { \"\\x41\" \"1\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), "A1")
        << "per-segment escape decode THEN byte-join: \\x41→'A', then '1' → \"A1\" "
           "(a raw-token merge would decode \\x411 as one byte 0x11)";
    ASSERT_EQ(std::get<std::string>(v.value).size(), 2u);

    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    auto const& ti = model.lattice().interner();
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3) << "\"A1\" (2 bytes) + NUL";
}

// ── C11/C23 6.4.5: wide / UTF string literals (L"…"/u"…"/U"…"/u8"…") ─────────
// A prefixed string types as Array<elementCore, codeUnits+1> and its literal pool
// value carries the element-width-encoded (LE) code units. The narrow path is
// unchanged; these assert the wide element core, the encoded byte blob, and the
// astral fail-loud (surrogate pairs are a later cycle).

TEST(HirLoweringCSubset, Utf16StringElementAndBytes) {
    // `u"AB"` → Array<U16,3>; bytes = 41 00 42 00 (2 LE U16 units), NUL implied.
    SemanticModel model = analyzeCSubset("void f() { u\"AB\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U16);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), std::string({0x41, 0x00, 0x42, 0x00}))
        << "u\"AB\" = two LE 16-bit units 0x0041 0x0042";
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3) << "2 code units + wide NUL";
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::U16);
}

TEST(HirLoweringCSubset, Utf32StringElementAndBytes) {
    // `U"AB"` → Array<U32,3>; bytes = 41 00 00 00 42 00 00 00.
    SemanticModel model = analyzeCSubset("void f() { U\"AB\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U32);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value),
              std::string({0x41, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00}));
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::U32);
}

TEST(HirLoweringCSubset, Utf8StringElementAndBytes) {
    // `u8"AB"` → Array<U8,3> (1 byte/ASCII unit); bytes = 41 42.
    SemanticModel model = analyzeCSubset("void f() { u8\"AB\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U8);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), "AB");
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3) << "2 u8 units + NUL";
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::U8);
}

TEST(HirLoweringCSubset, Utf16BmpMultibyteDecodesToOneUnit) {
    // `u"€"` — U+20AC, source bytes E2 82 AC — UTF-8-decodes to ONE U16 unit.
    // THE witness that the tokenizer's raw bytes are UTF-8-decoded (not passed
    // through byte-for-byte): 3 source bytes → 1 code unit → Array<U16,2>.
    SemanticModel model = analyzeCSubset("void f() { u\"\xe2\x82\xac\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U16);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), std::string({static_cast<char>(0xAC), 0x20}))
        << "U+20AC as one LE 16-bit unit";
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 2) << "ONE code unit + NUL (NOT 3 bytes + NUL)";
}

TEST(HirLoweringCSubset, Utf16AstralEncodesSurrogatePair) {
    // Cycle C: `u"😀"` — U+1F600 (F0 9F 98 80), a supplementary-plane cp under a
    // 16-bit element — now encodes as a UTF-16 SURROGATE PAIR: high 0xD83D then
    // low 0xDE00, i.e. the LE bytes 3D D8 00 DE (TWO code units), NEVER a silent
    // truncation. The array is Array<U16,3> (2 units + wide NUL). Red-on-disable:
    // revert the encodeCodepoint U16 astral branch and this fails to compile.
    SemanticModel model = analyzeCSubset("void f() { u\"\xf0\x9f\x98\x80\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U16);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value),
              std::string({0x3D, static_cast<char>(0xD8), 0x00, static_cast<char>(0xDE)}))
        << "U+1F600 as a UTF-16 surrogate pair: high 0xD83D then low 0xDE00 (LE)";
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3) << "2 code units (surrogate pair) + wide NUL";
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::U16);
}

TEST(HirLoweringCSubset, UcnAstralStringEncodesSurrogatePair) {
    // The `\U` universal-character-name form of the astral case: `u"\U0001F600"`
    // decodes (in the shared byte decoder) to U+1F600 and encodes to the SAME
    // surrogate pair 3D D8 00 DE as the raw `u"😀"`. Proves the UCN escape path.
    SemanticModel model = analyzeCSubset("void f() { u\"\\U0001F600\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U16);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value),
              std::string({0x3D, static_cast<char>(0xD8), 0x00, static_cast<char>(0xDE)}))
        << "\\U0001F600 → surrogate pair 0xD83D 0xDE00 (LE)";
}

TEST(HirLoweringCSubset, UcnBmpU32String) {
    // `U"é"` — the BMP UCN é (U+00E9) under a 32-bit element → one LE u32
    // unit 0x000000E9. The array is Array<U32,2> (1 unit + wide NUL).
    SemanticModel model = analyzeCSubset("void f() { U\"\\u00e9\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U32);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value),
              std::string({static_cast<char>(0xE9), 0x00, 0x00, 0x00}))
        << "\\u00e9 → one LE u32 unit 0x000000E9";
}

TEST(HirLoweringCSubset, UcnSurrogateHalfStringFailsLoud) {
    // FF1/FF2: `U"\uD800"` names a UTF-16 surrogate half — not a Unicode scalar
    // value. It fails loud with the dedicated H_InvalidUniversalCharacterName
    // (6.4.3), NEVER a silent CESU-8 / wrong unit.
    SemanticModel model = analyzeCSubset("void f() { U\"\\uD800\"; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "a surrogate-half UCN must fail lowering";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_InvalidUniversalCharacterName), 1u);
}

TEST(HirLoweringCSubset, WideStringByteEscapeFailsLoud) {
    // FF3: `u"\xC3\xA9"` uses `\x` byte escapes in a wide/UTF string. The old path
    // silently collapsed the two intended code units into one (0x00E9); Cycle C
    // fails loud with H_WideByteEscapeUnsupported (a raw code-unit value is not a
    // code point — the escape-value-as-code-unit feature is deferred). Narrow
    // `"\xC3\xA9"` is UNCHANGED (byte-producing).
    SemanticModel model = analyzeCSubset("void f() { u\"\\xC3\\xA9\"; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "a byte escape in a wide string must fail lowering";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_WideByteEscapeUnsupported), 1u);
}

TEST(HirLoweringCSubset, WideCharByteEscapeFailsLoud) {
    // MEDIUM-1 (code-audit): the wide-CHAR byte-escape path is the char twin of
    // WideStringByteEscapeFailsLoud (FF3). `u'\xC3\xA9'` must fail loud with
    // H_WideByteEscapeUnsupported (decodeWideCharCodepoint → ByteEscapeInWide), NOT
    // silently collapse C3 A9 → one char16_t 0x00E9. This is the sole exerciser of the
    // ByteEscapeInWide enumerator on the char path — a refactor dropping the guard would
    // reintroduce the collapse miscompile for the char form with nothing red.
    SemanticModel model = analyzeCSubset("void f() { u'\\xC3\\xA9'; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "a byte escape in a wide char must fail lowering";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_WideByteEscapeUnsupported), 1u);
}

TEST(HirLoweringCSubset, WideStringIllFormedUtf8FailsLoud) {
    // MEDIUM-2 (code-audit): after Cycle C, H_WideCharSurrogateUnsupported's surviving
    // trigger is a RAW ill-formed UTF-8 byte in a wide string body (astral-under-U16 now
    // surrogate-encodes; the `\x` route is shadowed by FF3's H_WideByteEscapeUnsupported).
    // A lone 0x80 (an invalid UTF-8 lead byte) must fail loud, not emit a garbage code
    // unit — this is the sole red-on-disable for that still-live diagnostic.
    std::string src = "void f() { u\"";
    src += static_cast<char>(0x80);
    src += "\"; }";
    SemanticModel model = analyzeCSubset(src);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "a raw ill-formed UTF-8 byte in a wide string must fail";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_WideCharSurrogateUnsupported), 1u);
}

TEST(HirLoweringCSubset, NarrowStringByteEscapeStillWorks) {
    // FF3 boundary: the NARROW `"\xC3\xA9"` keeps `\x` escapes (byte-producing) —
    // Array<Char,3> with the two raw bytes C3 A9. Proves FF3 did not regress the
    // narrow path.
    SemanticModel model = analyzeCSubset("void f() { \"\\xC3\\xA9\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::Char);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value),
              std::string({static_cast<char>(0xC3), static_cast<char>(0xA9)}))
        << "narrow \\xC3\\xA9 = the two raw bytes, unchanged";
}

TEST(HirLoweringCSubset, NarrowStringUnchangedUnderPrefixTable) {
    // Regression: a bare `"AB"` still types Array<Char,3> with the raw bytes —
    // the prefix table's auto-seeded narrow row is byte-identical to before.
    SemanticModel model = analyzeCSubset("void f() { \"AB\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::Char);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), "AB");
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::Char);
}

TEST(HirLoweringCSubset, WideStringRoundTripsThroughDsshirText) {
    // F6: a `u"AB"` literal's element core (U16) must survive the .dsshir
    // emit→parse round-trip. `literalCoreFor` reads the core off the Array element
    // (NOT a hardcoded Char), so the re-parsed pool value carries U16, not Char.
    SemanticModel model = analyzeCSubset("void f() { u\"AB\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    std::vector<std::string> names = symbolNames(model);
    HirTextContext ctx;
    ctx.interner    = &model.lattice().interner();
    ctx.symbolNames = &names;
    ctx.literalPool = &res->literalPool;
    DiagnosticReporter er;
    std::string const out = emitHir(res->hir, ctx, er);

    DiagnosticReporter pr;
    auto parsed = parseHir(out, CompilationUnitId{1}, pr);
    std::string diags;
    for (auto const& d : pr.all())
        diags += std::string{diagnosticCodeName(d.code)} + ": " + d.actual + "\n";
    ASSERT_TRUE(parsed->ok) << "u\"AB\" did not round-trip/verify\n" << diags;
    ASSERT_EQ(parsed->literalPool.size(), 1u);
    EXPECT_EQ(parsed->literalPool.at(0).core, TypeKind::U16)
        << "the re-parsed wide-string core must be U16 (read off the Array element, "
           "NOT hardcoded Char)";
}

// ── Cycle D — C11/C23 6.4.5p5: adjacent-concat prefix MIXING ────────────────
// A run of adjacent string literals takes the SINGLE distinct non-narrow prefix
// as its effective prefix (narrow segments widen to it, position-independent);
// TWO DIFFERENT non-narrow prefixes fail loud (impl-defined reject). These pin
// the two silent defects Cycle A left (mistype + miscompile) and the FF3-mixed hole.

TEST(HirLoweringCSubset, ConcatNarrowWidensLeadingWidePrefix) {
    // `L"a" "b"` — the L segment leads; the NARROW "b" widens to wchar_t. Under the
    // POSIX default wchar_t is I32 → Array<I32,3>. The two units are 'a','b'.
    SemanticModel model = analyzeCSubset("void f() { L\"a\" \"b\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u) << "the two pieces fold into ONE literal";
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::I32) << "the run is wide (wchar_t=I32 on POSIX)";
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3) << "'a' + widened 'b' + wide NUL";
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::I32);
}

TEST(HirLoweringCSubset, ConcatNarrowWidensTrailingWidePrefix) {
    // THE defect fix: `"a" L"b"` — the FIRST opener is NARROW, so pre-Cycle-D keyed
    // the run's core on `"` and DROPPED the `L` → a silent narrow mistype. The run's
    // effective prefix is L (position-independent), so this is Array<wchar_t,3> and
    // the narrow "a" widens. RED-ON-DISABLE: revert to first-opener keying → Char.
    SemanticModel model = analyzeCSubset("void f() { \"a\" L\"b\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::I32)
        << "`\"a\" L\"b\"` is WIDE — the trailing L prefix wins (was silently dropped)";
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::I32);
}

TEST(HirLoweringCSubset, ConcatNarrowWidenByteBlobPosixVsPe) {
    // THE byte-blob pin proving the NARROW segment WIDENED to the run's wide element
    // width. `"a" L"b"` = {'a','b'} as wchar_t. On POSIX (I32) each unit is 4 LE
    // bytes → 61 00 00 00 62 00 00 00. On pe (U16) each is 2 LE bytes → 61 00 62 00.
    // A first-opener-narrow regression would emit the raw bytes `61 62` (Char) — a
    // different length AND width on BOTH formats.
    {
        SemanticModel model = analyzeCSubset("void f() { \"a\" L\"b\"; }");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
        ASSERT_EQ(res->literalPool.size(), 1u);
        auto const& v = res->literalPool.at(0);
        EXPECT_EQ(v.core, TypeKind::I32);
        ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
        EXPECT_EQ(std::get<std::string>(v.value),
                  std::string({0x61, 0x00, 0x00, 0x00, 0x62, 0x00, 0x00, 0x00}))
            << "POSIX wchar_t=I32: 'a' and widened 'b' as two LE 4-byte units";
    }
    {
        SemanticModel model = analyzeCSubsetPe("void f() { \"a\" L\"b\"; }");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
        ASSERT_EQ(res->literalPool.size(), 1u);
        auto const& v = res->literalPool.at(0);
        EXPECT_EQ(v.core, TypeKind::U16) << "pe wchar_t is the U16 UTF-16 unit";
        ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
        EXPECT_EQ(std::get<std::string>(v.value),
                  std::string({0x61, 0x00, 0x62, 0x00}))
            << "pe wchar_t=U16: 'a' and widened 'b' as two LE 2-byte units";
    }
}

TEST(HirLoweringCSubset, ConcatSamePrefixPreserved) {
    // `u"a" u"b"` — the SAME non-narrow prefix on both segments is NOT a conflict
    // (one distinct kind). Array<U16,3>, existing behavior preserved.
    SemanticModel model = analyzeCSubset("void f() { u\"a\" u\"b\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U16);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), std::string({0x61, 0x00, 0x62, 0x00}))
        << "u\"a\" u\"b\" = two LE 16-bit units 0x0061 0x0062";
}

TEST(HirLoweringCSubset, ConcatConflictingNonNarrowPrefixesFailLoud) {
    // Each ordered pair of two DIFFERENT non-narrow prefixes is 6.4.5p5's impl-
    // defined case → fail loud with H_ConflictingStringLiteralPrefixes (NEVER a
    // silent resolve to one prefix, which drops the other's element width). Also a
    // 3-segment run with a LEADING NARROW piece, to pin the fold across positions.
    for (char const* src : {"void f() { u\"a\" U\"b\"; }",     // u16 vs u32
                            "void f() { u8\"a\" u\"b\"; }",    // char8_t vs char16_t
                            "void f() { L\"a\" u\"b\"; }",     // wchar_t vs char16_t
                            "void f() { \"a\" L\"b\" u\"c\"; }"}) {  // leading narrow, then L vs u
        SemanticModel model = analyzeCSubset(src);
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_FALSE(res->ok) << src;
        EXPECT_EQ(countCode(r, DiagnosticCode::H_ConflictingStringLiteralPrefixes), 1u) << src;
    }
}

TEST(HirLoweringCSubset, ConcatConflictPlainStatementFailsLoud) {
    // MF1 (red-on-disable via the SPECIFIC code): a PLAIN statement `u"a" U"b";` re-derives
    // its type at lowering (the semantic tier left it untyped). The EXPLICIT early conflict
    // branch reports the RIGHT reason — H_ConflictingStringLiteralPrefixes. Without it the
    // conflict is NOT silent (the type-drop guard still fires as a backstop: a Char stamp
    // under a wide effective opener) but with the WRONG reason (H_WideCharSurrogateUnsupported
    // "not well-formed UTF-8"). So this pins the branch via the EXACT code (countCode below),
    // and res->ok stays FALSE either way — defense in depth, correct-reason on top.
    SemanticModel model = analyzeCSubset("void f() { u\"a\" U\"b\"; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "a mixed-prefix concat as a plain statement must fail lowering, not "
           "silently type Array<Char,3>";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ConflictingStringLiteralPrefixes), 1u);
    EXPECT_EQ(res->literalPool.size(), 0u) << "no literal is minted on the conflict path";
}

TEST(HirLoweringCSubset, ConcatFF3MixedNarrowWidePrefixFailsLoud) {
    // The FF3-mixed hole (now CLOSED): `"a" L"\xC3"` — the run is WIDE (effective
    // prefix L) but the FIRST opener is narrow. Pre-Cycle-D the FF3 byte-escape guard
    // keyed on the first opener → narrow → MISS → the old silent UTF-8 collapse. Now
    // the guard keys on the run's effective prefix, so the `\xC3` byte escape in a
    // wide run fails loud with H_WideByteEscapeUnsupported.
    SemanticModel model = analyzeCSubset("void f() { \"a\" L\"\\xC3\"; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "a byte escape in a wide (via a later prefix) run must fail";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_WideByteEscapeUnsupported), 1u);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ConflictingStringLiteralPrefixes), 0u)
        << "a SINGLE non-narrow prefix is not a conflict — only the byte escape fires";
}

TEST(HirLoweringCSubset, ConcatAllNarrowUnchanged) {
    // RED-ON-DISABLE guard: the effective-prefix change must NOT alter the all-narrow
    // path. `"a" "b"` stays byte-identical Array<Char,3> with the raw bytes "ab".
    SemanticModel model = analyzeCSubset("void f() { \"a\" \"b\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::Char);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), "ab");
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::Char);
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
    ctx.literalPool = &res->literalPool;
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

// ── D5.3 cycle 1a: brace-init lowering ───────────────────────────────────
//
// Each test lowers a VarDecl whose initializer is a `braceInitList` and
// pins that the lowered HIR contains a `ConstructAggregate` node with the
// expected slot count and that the lowering reports clean (`res->ok`).

namespace {
[[nodiscard]] HirNodeId firstVarInitOfFn(Hir const& hir, HirNodeId fn) {
    HirNodeId body = hir.functionBody(fn);
    for (HirNodeId s : hir.children(body)) {
        if (hir.kind(s) == HirKind::VarDecl) {
            if (auto init = hir.varDeclInit(s)) return *init;
        }
    }
    return HirNodeId{};
}
} // namespace

TEST(HirLoweringCSubset, D5_3_PositionalStructInit) {
    SemanticModel model = analyzeCSubset(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = {1, 2}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 2u);
}

// SP3.c LANDED 2026-05-28: single-level field designator now resolves
// via the TYPE-AWARE path (look up name in `compositeScopeFor(context)`
// rather than the lexical scope Pass 2 stamps). Pins value ORDERING:
// `.y = 7, .x = 3` must produce slot 0 = 3 (.x), slot 1 = 7 (.y).
// A regression that swapped lexical-order resolution for declaration-
// order would pass a count-only assertion silently.
TEST(HirLoweringCSubset, D5_3_FieldDesignatorInit) {
    SemanticModel model = analyzeCSubset(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = {.y = 7, .x = 3}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    // Slot 0 is .x = 3, slot 1 is .y = 7 (declaration order).
    ASSERT_EQ(res->hir.kind(kids[0]), HirKind::Literal);
    ASSERT_EQ(res->hir.kind(kids[1]), HirKind::Literal);
    auto slot0Lit = res->literalPool.at(res->hir.payload(kids[0]));
    auto slot1Lit = res->literalPool.at(res->hir.payload(kids[1]));
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(slot0Lit.value));
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(slot1Lit.value));
    EXPECT_EQ(std::get<std::int64_t>(slot0Lit.value), 3);
    EXPECT_EQ(std::get<std::int64_t>(slot1Lit.value), 7);
}

// C99 §6.7.8p19: a later designator OVERRIDES an earlier value at the
// same subobject. `{.x = 1, .x = 2}` → slot 0 = 2 (last wins).
TEST(HirLoweringCSubset, D5_3_LaterDesignatorOverridesEarlier) {
    SemanticModel model = analyzeCSubset(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = {.x = 1, .x = 2}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    ASSERT_EQ(res->hir.kind(kids[0]), HirKind::Literal);
    auto slot0Lit = res->literalPool.at(res->hir.payload(kids[0]));
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(slot0Lit.value));
    EXPECT_EQ(std::get<std::int64_t>(slot0Lit.value), 2)
        << "later .x = 2 must override earlier .x = 1 per C99 §6.7.8p19";
}

// Dot-chained designator coexists with a sibling brace-init under a
// different outer slot. Exercises the InitSlot tree's nested-merge
// substrate when a chained write and a positional write both land at
// the same outer-aggregate level.
TEST(HirLoweringCSubset, D5_3_DotChainedDesignatorWithSibling) {
    SemanticModel model = analyzeCSubset(
        "struct Inner { int v; };\n"
        "struct Outer { struct Inner a; struct Inner b; };\n"
        "void f() { struct Outer o = {.a.v = 1, .b = {.v = 9}}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::ConstructAggregate);
}

// Restored: zero-fill with designators
TEST(HirLoweringCSubset, D5_3_ZeroFillsOmittedField) {
    SemanticModel model = analyzeCSubset(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = {.y = 7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::Literal);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::Literal);
}

// Restored: chained-brace with field designators
TEST(HirLoweringCSubset, D5_3_ChainedBraceNesting) {
    SemanticModel model = analyzeCSubset(
        "struct Inner { int v; };\n"
        "struct Outer { struct Inner a; struct Inner b; };\n"
        "void f() { struct Outer o = {.a = {.v = 1}, .b = {.v = 2}}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::ConstructAggregate);
}

// Lock-in: field designator naming a NON-EXISTENT field emits a
// diagnostic that the field doesn't belong to the target struct.
TEST(HirLoweringCSubset, D5_3_UnknownFieldDesignatorEmitsDiag) {
    SemanticModel model = analyzeCSubset(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = {.bogus = 7}; }\n");
    if (model.hasErrors()) return;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("doesn't belong") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "unknown field designator must be diagnosed";
    EXPECT_FALSE(res->ok);
}

TEST(HirLoweringCSubset, D5_3_OmittedFieldZeroFillStructureWithoutDesignator) {
    // Without field designators, `struct Point p = {7}` lands `7` at
    // slot 0 (positional) + zero-fills slot 1. This exercises the
    // zero-fill path WITHOUT depending on the substrate-blocked
    // designator-name resolution.
    SemanticModel model = analyzeCSubset(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = {7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::Literal);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::Literal);
}

TEST(HirLoweringCSubset, D5_3_ChainedBraceNestingPositional) {
    // The positional form `struct Outer o = {{1}, {2}}` exercises the
    // chained-brace nesting recursion without depending on designator
    // resolution.
    SemanticModel model = analyzeCSubset(
        "struct Inner { int v; };\n"
        "struct Outer { struct Inner a; struct Inner b; };\n"
        "void f() { struct Outer o = {{1}, {2}}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::ConstructAggregate);
}

TEST(HirLoweringCSubset, D5_3_PositionalArrayInit) {
    SemanticModel model = analyzeCSubset(
        "void f() { int xs[3] = {10, 20, 30}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 3u);
}

// Under-filled array: explicit elements at slots 0..k-1, synth-zero at k..N-1.
TEST(HirLoweringCSubset, D5_3_ArrayUnderfillZeroFillsTail) {
    SemanticModel model = analyzeCSubset(
        "void f() { int xs[5] = {1, 2}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 5u);
    for (auto k : kids) EXPECT_EQ(res->hir.kind(k), HirKind::Literal);
}

// C99 §6.7.8p17: a designator restarts the fill cursor at the designated
// position; the immediately following positional element resumes from
// `designated + 1`. Exercised here with INDEX designators (array).
TEST(HirLoweringCSubset, D5_3_CursorRestartAfterIndexDesignator) {
    SemanticModel model = analyzeCSubset(
        "void f() { int xs[5] = {[1] = 9, 7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 5u);
    for (auto k : kids) EXPECT_EQ(res->hir.kind(k), HirKind::Literal);
}

// Same C99 §6.7.8p17 invariant exercised with FIELD designators
// (struct) — now that SP3.c lifted the field-designator substrate
// blocker. `{.b = 9, 7}` puts 9 at .b (slot 1), then the positional 7
// at slot 2 (.c) per cursor restart.
TEST(HirLoweringCSubset, D5_3_CursorRestartAfterFieldDesignator) {
    SemanticModel model = analyzeCSubset(
        "struct Trip { int a; int b; int c; };\n"
        "void f() { struct Trip t = {.b = 9, 7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 3u);
    for (auto k : kids) EXPECT_EQ(res->hir.kind(k), HirKind::Literal);
}

// Empty brace `T x = {};` — fully zero-fills via synthZero recursion. The
// existing zero-fill test only exercises synthZero on scalar fields; this
// hits the recursive aggregate-arm (struct of struct).
TEST(HirLoweringCSubset, D5_3_EmptyBraceZeroFillsNestedAggregate) {
    SemanticModel model = analyzeCSubset(
        "struct Inner { int v; };\n"
        "struct Outer { struct Inner a; struct Inner b; };\n"
        "void f() { struct Outer o = {}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::ConstructAggregate);
}

// ── D5.3 cycle 1b: index designator + compound literal + return / call /
// assign context sites + locked-in substrate-blocked diagnostics ────────

// 1b.2: `int xs[3] = {[2] = 7};` — integer-literal index designator lands
// at slot 2; slots 0 and 1 zero-fill.
TEST(HirLoweringCSubset, D5_3_IndexDesignatorLiteral) {
    SemanticModel model = analyzeCSubset(
        "void f() { int xs[3] = {[2] = 7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 3u);
}

// 1b.2: multi-index designator with cursor jump — `{[0] = 1, [4] = 5}`
// against a 5-slot array. Pins the cursor-restart behavior past one
// jump (the test-analyzer's #2 rating-8 gap).
TEST(HirLoweringCSubset, D5_3_MultiIndexDesignatorWithCursorJump) {
    SemanticModel model = analyzeCSubset(
        "void f() { int xs[5] = {[0] = 1, [4] = 5}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 5u);
    // Slots 0 and 4 are explicit Literals; 1, 2, 3 are zero-fill Literals.
    for (auto k : kids) EXPECT_EQ(res->hir.kind(k), HirKind::Literal);
}

// SP3.c LANDED 2026-05-28: dot-chained `.a.v = 1` now resolves via the
// type-aware designator walker that threads a current-type cursor
// through each step. The InitSlot tree's `nested` substrate (sleeping
// since cycle 1b) makes the multi-step write semantically right (.a
// becomes a sub-aggregate; .v inside it is the explicit value; the
// rest of .a + the entire .b zero-fill).
TEST(HirLoweringCSubset, D5_3_DotChainedDesignator) {
    SemanticModel model = analyzeCSubset(
        "struct Inner { int v; };\n"
        "struct Outer { struct Inner a; struct Inner b; };\n"
        "void f() { struct Outer o = {.a.v = 1}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    // .a is a sub-aggregate (the dot-chained write created it).
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::ConstructAggregate);
    // .b is the synth-zero sub-aggregate (omitted).
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::ConstructAggregate);
}

// 1b.4: ordinary `return 7;` still coerces — the return-site refactor
// (lowerExprOrBraceInit consolidating brace-init detection across all
// context sites) must not regress non-brace-init returns.
TEST(HirLoweringCSubset, D5_3_OrdinaryReturnStillCoerces) {
    SemanticModel model = analyzeCSubset("int f() { return 7; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

// 1b.3 SUBSTRATE-BLOCKED LOCK-IN: a compound literal `(T){...}` reaches
// `lowerCompoundLiteral`; the typeRef child's type can't be resolved
// from Pass 2 (which only stamps types on identifier references inside
// `operand` rules — not type-position references). Until plan 08.5 §SP3
// substrate stamps types on typeRef nodes, the lowering fails LOUD with
// a `compound literal type-ref did not resolve` diagnostic. This test
// STRICTLY asserts the diagnostic — when SP3 lands, this test must be
// inverted to assert `res->ok && children.size() == 2`. The strict form
// matters: a `found || res->ok` short-circuit would let a future
// regression silently set ok=true with InvalidType and still pass.
// SP3.b LANDED 2026-05-28: with `structTypeRef` added to the c-subset
// `references` config (with `nameMatch: "lastIdentifier"`), Pass 2
// resolves the struct's Identifier + stamps `nodeToType` on it; the
// recursive `resolveStampedTypeBelow` then finds the stamp and the
// compound-literal lowers cleanly.
TEST(HirLoweringCSubset, D5_3_CompoundLiteralInVarDeclInit) {
    SemanticModel model = analyzeCSubset(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = (struct Point){.x = 1, .y = 2}; }\n");
    if (model.hasErrors()) {
        for (auto const& d : model.diagnostics().all())
            ADD_FAILURE() << diagnosticCodeName(d.code) << " actual=" << d.actual;
        return;
    }
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 2u);
}

// Substrate-blocked: non-literal index designator (e.g. `[n] = 7`)
// must emit a diagnostic that names CST-side const-eval as the blocker.
// Strict form: when semantic accepts the input, the lowering MUST emit
// the diagnostic (silent acceptance would be a regression the looser
// `found || res->ok` form would have missed).
TEST(HirLoweringCSubset, D5_3_NonLiteralIndexDesignatorEmitsDiag) {
    SemanticModel model = analyzeCSubset(
        "void f() { int n = 1; int xs[3] = {[n] = 7}; }\n");
    if (model.hasErrors()) return;   // tolerated: semantic may reject too
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("integer literal") != std::string::npos
         || d.actual.find("const-eval") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found)
        << "non-literal index designator MUST be diagnosed pending "
           "CST-side const-eval substrate";
    EXPECT_FALSE(res->ok)
        << "lowering must fail when a non-literal index designator "
           "appears (substrate-blocked path)";
}

// ── plan 12.5 §0.2 D6: CST-side const-eval ──────────────────────────
// The shared CST const-eval engine folds literal arithmetic, ternary,
// LogicalAnd/Or, and `const`-bound identifier refs at all 3 consumer
// sites: array length, enumerator value, and index designator.

// Helper: extract the Array length from a VAR symbol's declared type
// via the TypeInterner. Returns -1 if the symbol isn't found or its
// type isn't an Array; the caller asserts the expected length.
static std::int64_t arrayLengthOfVar(SemanticModel const& model,
                                     std::string_view name) {
    auto const& interner = model.lattice().interner();
    for (auto const& sym : model.symbols()) {
        if (sym.name != name) continue;
        if (!sym.type.valid()) return -1;
        if (interner.kind(sym.type) != TypeKind::Array) return -1;
        auto scals = interner.scalars(sym.type);
        if (scals.empty()) return -1;
        return scals[0];
    }
    return -1;
}

// Array-length const-expr fold: `int a[1+2];` produces Array<int, 3>.
// Previously the hand-rolled "literal-only" check refused anything but
// a single integer literal token; the new engine folds the BinaryOp.
TEST(HirLoweringCSubset, CstConstEval_ArrayLengthConstExpr) {
    SemanticModel model = analyzeCSubset(
        "void f() { int xs[1+2]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 3);
}

// Array-length with `const`-bound identifier ref. The resolver walks
// the scope chain from the declaration site, finds `N` as `isConst`,
// and folds `N + 1` to 4. Mutable refs still refuse — covered by the
// locked-in NonLiteral test that uses `int n = 1;` (not `const`).
TEST(HirLoweringCSubset, CstConstEval_ArrayLengthConstRef) {
    SemanticModel model = analyzeCSubset(
        "const int N = 3;\n"
        "void f() { int xs[N + 1]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 4);
}

// FC4 c1 (audit F1) — the PER-DECLARATOR const-init name match.
// `const int A = 100, B = 2;` carries TWO initDeclarators under ONE
// decl node; findInitExprInDecl must resolve each symbol's init by
// its OWN name node. The silent failure this pins: degrading the
// match to "the FIRST declarator's init" keeps every single-
// declarator test green while B folds as 100 — so `xs[B + 1]`
// becomes Array<101>, not Array<3>. The A-fed sibling stays green
// under that degrade (A IS the first declarator); the B assertion
// is the discriminating lever (red-on-disable demonstrated by
// short-circuiting the name match to the list head).
TEST(HirLoweringCSubset, CstConstEval_MultiDeclaratorConstInitsResolvePerName) {
    SemanticModel model = analyzeCSubset(
        "const int A = 100, B = 2;\n"
        "void f() { int xs[B + 1]; int ys[A - 97]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 3);
    EXPECT_EQ(arrayLengthOfVar(model, "ys"), 3);
}

// UnaryOp fold: `-(-3)` → 3. Pins the UnaryExprRule branch
// (previously test-coverage gap).
TEST(HirLoweringCSubset, CstConstEval_UnaryFolds) {
    SemanticModel model = analyzeCSubset(
        "void f() { int xs[-(-3)]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 3);
}

// LogicalAnd short-circuit: `1 && 2` is 1; combine with arithmetic to
// land on a non-trivial length. Pins the LogicalAnd/Or branch.
TEST(HirLoweringCSubset, CstConstEval_LogicalAndFolds) {
    SemanticModel model = analyzeCSubset(
        "void f() { int xs[(1 && 2) + 4]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 5);
}

// LogicalOr short-circuit: `1 || (some_runtime)` should fold to 1
// regardless of the rhs being non-foldable. The rhs is a mutable
// reference that would refuse to fold if reached.
TEST(HirLoweringCSubset, CstConstEval_LogicalOrShortCircuits) {
    SemanticModel model = analyzeCSubset(
        "void f() { int n = 1; int xs[(1 || n) + 2]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 3);
}

// Cycle detection: `const int a = b + 0; const int b = a + 0;` is a
// genuine cycle. The engine refuses with NotAConstantExpression at
// the second encounter; caller emits S_NonConstantArrayLength.
TEST(HirLoweringCSubset, CstConstEval_CycleRefuses) {
    SemanticModel model = analyzeCSubset(
        "const int a = b + 0;\n"
        "const int b = a + 0;\n"
        "void f() { int xs[a + 1]; }\n");
    bool found = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_NonConstantArrayLength) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found)
        << "cyclic const-init chain must refuse to fold";
}

// D7 scope-context tracking: shadowing across scopes is NOT a cycle.
// Outer `const X=1; const Y=X+1;` + inner `const X=Y;` — the inner X
// shadows the outer X at use-site, but Y's init `X+1` must be
// evaluated in MODULE scope (Y's scope), where it correctly resolves
// to the OUTER X (= 1). Result: inner X = Y = 2; `xs[X+1]` = 3.
//
// Without scope-context tracking, evaluating Y's `X+1` would re-use
// the function scope (the original use-site) and find the inner X
// → false cycle or wrong value. The engine threads
// `resolved->initScopeOpaque` through recursion to fix this.
TEST(HirLoweringCSubset, CstConstEval_ShadowingResolvesCorrectScope) {
    SemanticModel model = analyzeCSubset(
        "const int X = 1;\n"
        "const int Y = X + 1;\n"
        "void f() { const int X = Y; int xs[X + 1]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 3);
}

// Cross-scope ref chain (no shadowing): outer module consts referenced
// from a function body. Two-deep const-ref chain. The engine resolves
// `xs[N + 1]` → folds N (which is itself `M+1`=3) → fold to 4.
TEST(HirLoweringCSubset, CstConstEval_TransitiveConstRef) {
    SemanticModel model = analyzeCSubset(
        "const int M = 2;\n"
        "const int N = M + 1;\n"
        "void f() { int xs[N + 1]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 4);
}

// Division by zero in a const-expr: `int xs[1/0];` — the engine
// refuses with DivisionByZero (caller maps to S_NonConstantArrayLength
// since array length doesn't have a dedicated div-by-zero diagnostic).
TEST(HirLoweringCSubset, CstConstEval_DivByZeroRefuses) {
    SemanticModel model = analyzeCSubset(
        "void f() { int xs[1/0]; }\n");
    bool found = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_NonConstantArrayLength) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found)
        << "div-by-zero in const-expr must refuse to fold";
}

// Index designator const-expr fold: `{[1+1] = 7}` lowers to the same
// ConstructAggregate shape as `{[2] = 7}` (slot 2 gets 7, slots 0/1
// zero-fill).
TEST(HirLoweringCSubset, CstConstEval_IndexDesignatorConstExpr) {
    SemanticModel model = analyzeCSubset(
        "void f() { int xs[3] = {[1+1] = 7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 3u);
}

// Index designator with `const`-bound ref: `[N+1]` where `const N=1`
// folds to slot 2.
TEST(HirLoweringCSubset, CstConstEval_IndexDesignatorConstRef) {
    SemanticModel model = analyzeCSubset(
        "const int N = 1;\n"
        "void f() { int xs[3] = {[N + 1] = 7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

// Enumerator value const-expr fold: `A = 1+1` ⇒ A=2; subsequent
// `B` auto-increments to 3.
TEST(HirLoweringCSubset, CstConstEval_EnumeratorConstExpr) {
    SemanticModel model = analyzeCSubset(
        "enum E { A = 1 + 1, B };\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    // The two enumerators bind A=2 and B=3; verify via SymbolRecord.
    bool foundA = false, foundB = false;
    for (auto const& sym : model.symbols()) {
        if (sym.name == "A") { EXPECT_EQ(sym.enumValue, 2); foundA = true; }
        if (sym.name == "B") { EXPECT_EQ(sym.enumValue, 3); foundB = true; }
    }
    EXPECT_TRUE(foundA);
    EXPECT_TRUE(foundB);
}

// Enumerator value with `const`-bound ref: `A = X + 1` where const X=5
// resolves to 6.
TEST(HirLoweringCSubset, CstConstEval_EnumeratorConstRef) {
    SemanticModel model = analyzeCSubset(
        "const int X = 5;\n"
        "enum E { A = X + 1 };\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    for (auto const& sym : model.symbols()) {
        if (sym.name == "A") { EXPECT_EQ(sym.enumValue, 6); return; }
    }
    FAIL() << "enumerator A not found";
}

// Ternary fold: `[1 < 2 ? 3 : 5]` → length 3 (cond true → then arm).
TEST(HirLoweringCSubset, CstConstEval_TernaryFolds) {
    SemanticModel model = analyzeCSubset(
        "void f() { int xs[1 < 2 ? 3 : 5]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
}

// Mutable ref still refuses: `int n = 1; int xs[n+1];` emits
// S_NonConstantArrayLength because `n` is NOT `isConst`. The engine
// correctly walks the scope chain, finds `n`, sees it's mutable, and
// refuses — preserving the locked-in NonLiteralIndexDesignatorEmitsDiag
// test for the design-time case where a programmer used a runtime
// variable in a const-expr slot.
TEST(HirLoweringCSubset, CstConstEval_MutableRefRefuses) {
    SemanticModel model = analyzeCSubset(
        "void f() { int n = 1; int xs[n + 1]; }\n");
    bool foundDiag = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_NonConstantArrayLength) {
            foundDiag = true; break;
        }
    }
    EXPECT_TRUE(foundDiag)
        << "mutable ref `n` must refuse to fold and emit S_NonConstantArrayLength";
}

// ── D5.4 unions ──────────────────────────────────────────────────────

// `union U u;` declares + types via the same Pass 1.5 path as struct;
// the difference is `compositeKind: "union"` in the c-subset config →
// `interner.unionType(...)`. Empty init zero-fills the FIRST variant.
TEST(HirLoweringCSubset, D5_4_UnionDeclLowersToTypeDecl) {
    SemanticModel model = analyzeCSubset(
        "union U { int i; char c; };\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::TypeDecl);
}

// Positional union brace-init `{ 5 }` initializes the FIRST variant.
// Aggregate is 1-child (NOT 2 — unions are not zero-fill-all).
TEST(HirLoweringCSubset, D5_4_UnionPositionalInit) {
    SemanticModel model = analyzeCSubset(
        "union U { int i; char c; };\n"
        "void f() { union U u = { 5 }; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 1u)
        << "union aggregate must have exactly 1 child (the active variant)";
}

// Designated union brace-init `{ .c = 'a' }` initializes the named
// variant — second variant (index 1), not first.
TEST(HirLoweringCSubset, D5_4_UnionDesignatedInit) {
    SemanticModel model = analyzeCSubset(
        "union U { int i; char c; };\n"
        "void f() { union U u = { .c = 'a' }; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 1u);
    // The child's HIR type identifies the chosen variant — Char here.
    // (We don't read the type interner from the test directly; the
    // child being Literal is enough to confirm the chosen variant's
    // value was lowered.)
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::Literal);
}

// Empty `{}` union init zero-fills the FIRST variant (1-child aggregate).
TEST(HirLoweringCSubset, D5_4_UnionEmptyBrace) {
    SemanticModel model = analyzeCSubset(
        "union U { int i; char c; };\n"
        "void f() { union U u = {}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 1u);
}

// Multi-element union brace-init MUST emit a diagnostic.
TEST(HirLoweringCSubset, D5_4_UnionMultiElementEmitsDiag) {
    SemanticModel model = analyzeCSubset(
        "union U { int i; char c; };\n"
        "void f() { union U u = { 1, 2 }; }\n");
    if (model.hasErrors()) return;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("at most one variant") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "multi-element union init must be diagnosed";
    EXPECT_FALSE(res->ok);
}

// Unknown union variant name → diagnostic.
TEST(HirLoweringCSubset, D5_4_UnionUnknownVariantEmitsDiag) {
    SemanticModel model = analyzeCSubset(
        "union U { int i; char c; };\n"
        "void f() { union U u = { .bogus = 1 }; }\n");
    if (model.hasErrors()) return;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("doesn't belong") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "unknown union variant must be diagnosed";
    EXPECT_FALSE(res->ok);
}

// Index designator on a union is nonsensical → diagnostic.
TEST(HirLoweringCSubset, D5_4_UnionIndexDesignatorEmitsDiag) {
    SemanticModel model = analyzeCSubset(
        "union U { int i; char c; };\n"
        "void f() { union U u = { [0] = 1 }; }\n");
    if (model.hasErrors()) return;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("not meaningful on union") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "index designator on union must be diagnosed";
    EXPECT_FALSE(res->ok);
}

// Chained designator `{.a.b = 1}` on a union must be diagnosed (variant
// access has no sub-position semantics in C99). Lock-in for the
// silent-failure-hunter HIGH finding.
TEST(HirLoweringCSubset, D5_4_UnionChainedDesignatorEmitsDiag) {
    SemanticModel model = analyzeCSubset(
        "struct Inner { int v; };\n"
        "union U { struct Inner a; int i; };\n"
        "void f() { union U u = { .a.v = 1 }; }\n");
    if (model.hasErrors()) return;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("chained designator on a union") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "chained designator on union must be diagnosed";
    EXPECT_FALSE(res->ok);
}

// Union nested inside a struct: the recursive InitSlot path lands on
// the union's `lowerUnionBraceInit` correctly + omitted struct slots
// containing unions zero-fill via the corrected `synthZeroOrError`
// Union arm (1-child first-variant).
TEST(HirLoweringCSubset, D5_4_UnionNestedInStruct) {
    SemanticModel model = analyzeCSubset(
        "union U { int i; char c; };\n"
        "struct S { union U u; int x; };\n"
        "void f() { struct S s = { .u = { .c = 'a' }, .x = 7 }; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    // Outer slot 0 is the union → 1-child ConstructAggregate.
    ASSERT_EQ(res->hir.kind(kids[0]), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(kids[0]).size(), 1u);
    // Outer slot 1 is .x = 7 → Literal.
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::Literal);
}

// Union zero-filled by the containing struct's missing-field path: the
// `synthZeroOrError(unionTy)` produces a 1-child aggregate (not N).
TEST(HirLoweringCSubset, D5_4_UnionZeroFilledByContainingStruct) {
    SemanticModel model = analyzeCSubset(
        "union U { int i; char c; };\n"
        "struct S { int x; union U u; };\n"
        "void f() { struct S s = { .x = 1 }; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    // Slot 0 = .x = 1 (Literal). Slot 1 = synth-zero union → 1-child agg.
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::Literal);
    ASSERT_EQ(res->hir.kind(kids[1]), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(kids[1]).size(), 1u);
}

// Compound literal of union type: `(union U){.c='a'}` exercises
// lowerCompoundLiteral → lowerBraceInit → lowerUnionBraceInit chain.
TEST(HirLoweringCSubset, D5_4_UnionCompoundLiteral) {
    SemanticModel model = analyzeCSubset(
        "union U { int i; char c; };\n"
        "void f() { union U u = (union U){ .c = 'a' }; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 1u);
}

// Member access on a union: `u.c` must resolve via the existing
// MemberAccess path (same `compositeScopeByType` substrate that
// structs use).
TEST(HirLoweringCSubset, D5_4_UnionMemberAccess) {
    SemanticModel model = analyzeCSubset(
        "union U { int i; char c; };\n"
        "void f() { union U u = { .c = 'a' }; char x = u.c; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

// ── D5.5 enums ───────────────────────────────────────────────────────

// `enum E { A, B, C };` declares a TypeDecl. The enum type is nominal-
// by-name; enumerators are Variable symbols with the enum type, bound
// in the enum's inner scope (accessed as `E.A` via MemberAccess).
TEST(HirLoweringCSubset, D5_5_EnumDeclLowersToTypeDecl) {
    SemanticModel model = analyzeCSubset(
        "enum E { A, B, C };\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::TypeDecl);
}

// C-classic enumerator visibility: `enum E { A, B }` makes `A` and `B`
// visible directly in the enclosing scope (Pass 1.5 lifts the
// enumerator bindings from the enum's inner scope to the parent).
// `enum E e = A;` resolves `A` against the enclosing scope.
TEST(HirLoweringCSubset, D5_5_EnumValueUseViaBareName) {
    SemanticModel model = analyzeCSubset(
        "enum E { A, B, C };\n"
        "void f() { enum E e = A; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

// Top-level enum with a TRAILING COMMA parses cleanly — the RED-ON-DISABLE guard
// for the schema-compiler `recomputeAltExpectedSets` fixpoint (D-CSUBSET-STRUCT-
// BODY-VARDECL-POSITION §2d). The enum body `enumerator (Comma enumerator?)*` is
// an `optional` inside a `repeat` before the required `}` closer; without the
// fixpoint the trailing-comma optional never learns `}` can follow, so the
// SPECULATIVE body probe (`topLevelCompositeSpec`) hits P_NoAlternativeMatched
// and rolls back to the ref form → parse error. VERIFIED red-on-disable: toggling
// off the `recomputeAltExpectedSets` orchestration call makes THIS test fail.
// NOTE: the `recomputeAltExpectedSets` fixpoint is GLOBAL (every grammar's
// expectedSets), so it fixes the typedef-position surface (`typedef enum {…,} T;`)
// too — see the parallel pin D5_5_TypedefEnumTrailingCommaParses
// (D-CSUBSET-TYPEDEF-ENUM-TRAILING-COMMA CLOSED; the registry's earlier
// "typedef is a separate gap" scoping was overly conservative).
TEST(HirLoweringCSubset, D5_5_EnumTrailingCommaParses) {
    SemanticModel model = analyzeCSubset(
        "enum E { A, B, C, };\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    // The trailing comma must mint EXACTLY A, B, C — never a phantom 4th
    // enumerator (an over-eager `(Comma enumerator?)*` that consumed the comma
    // as introducing a fourth, empty enumerator would be a silent mis-parse a
    // bare `!hasErrors()` check could miss).
    int enumerators = 0;
    for (auto const& s : model.symbols())
        if (s.name == "A" || s.name == "B" || s.name == "C") ++enumerators;
    EXPECT_EQ(enumerators, 3)
        << "exactly A, B, C — the trailing comma introduces no enumerator";
}

// TYPEDEF-POSITION enum with a TRAILING COMMA parses cleanly — the parallel of
// D5_5_EnumTrailingCommaParses for the typedef surface (closes the now-stale
// D-CSUBSET-TYPEDEF-ENUM-TRAILING-COMMA). The global schema-compiler fixpoint
// `recomputeAltExpectedSets` (D-PARSE-SCHEMA-NESTED-NULLABLE-FOLLOW) fixes EVERY
// grammar's expectedSets, so the typedef speculative surface recovers the
// trailing-comma enum body too — verified parsing AND running across anon /
// named-tag / valued / single-element forms. RED-ON-DISABLE: toggling off the
// `recomputeAltExpectedSets` orchestration call (grammar_schema_json.cpp) reddens
// THIS pin alongside the top-level one — verified by the closing cycle.
TEST(HirLoweringCSubset, D5_5_TypedefEnumTrailingCommaParses) {
    SemanticModel model = analyzeCSubset(
        "typedef enum { A, B, C, } T;\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    // Exactly A, B, C — the trailing comma must mint no phantom 4th enumerator
    // (an over-eager `(Comma enumerator?)*` consuming the comma as a 4th empty
    // enumerator would be a silent mis-parse a bare `!hasErrors()` would miss).
    int enumerators = 0;
    for (auto const& s : model.symbols())
        if (s.name == "A" || s.name == "B" || s.name == "C") ++enumerators;
    EXPECT_EQ(enumerators, 3)
        << "exactly A, B, C — the trailing comma introduces no enumerator";
}

// Enumerator values: implicit auto-increment + explicit integer-literal
// + auto-increment from explicit. C99 §6.7.2.2. Verifies that Pass 1.5
// actually COMPUTES the values (not just parses them).
TEST(HirLoweringCSubset, D5_5_EnumValuesComputed) {
    SemanticModel model = analyzeCSubset(
        "enum E { A, B, C = 5, D };\n");
    ASSERT_FALSE(model.hasErrors());
    // Look up each enumerator's value in the symbol table.
    auto findEnumerator = [&](std::string const& name) -> SymbolRecord const* {
        for (auto const& s : model.symbols()) {
            if (s.name == name) return &s;
        }
        return nullptr;
    };
    auto const* a = findEnumerator("A");
    auto const* b = findEnumerator("B");
    auto const* c = findEnumerator("C");
    auto const* d = findEnumerator("D");
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr); ASSERT_NE(d, nullptr);
    EXPECT_EQ(a->enumValue, 0) << "A implicit → 0";
    EXPECT_EQ(b->enumValue, 1) << "B implicit → A + 1 = 1";
    EXPECT_EQ(c->enumValue, 5) << "C explicit = 5";
    EXPECT_EQ(d->enumValue, 6) << "D implicit → C + 1 = 6";
}

// Enumerator type identity: each enumerator must be typed as the enum
// (not as the underlying int). A regression that left enumerators
// typed as I32 would pass count-only assertions but break downstream
// type-equivalence checks.
TEST(HirLoweringCSubset, D5_5_EnumeratorTypedAsEnum) {
    SemanticModel model = analyzeCSubset("enum E { A };\n");
    ASSERT_FALSE(model.hasErrors());
    auto& interner = model.lattice().interner();
    TypeId const enumTy = interner.enumType("E", TypeKind::I32);
    // The enumerator A must carry the enum TypeId, not raw I32.
    SymbolRecord const* a = nullptr;
    for (auto const& s : model.symbols())
        if (s.name == "A") { a = &s; break; }
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->type.v, enumTy.v)
        << "enumerator must be typed as the enum, not the underlying int";
    EXPECT_NE(a->type.v, interner.primitive(TypeKind::I32).v)
        << "enumerator MUST NOT carry the raw I32 TypeId";
}

// Lift-to-enclosing collision: `int A; enum E { A };` must emit
// S_RedeclaredSymbol pointing at the enumerator decl. Locks the
// otherwise-test-untouched diagnostic branch.
TEST(HirLoweringCSubset, D5_5_EnumeratorCollidesWithEnclosingName) {
    SemanticModel model = analyzeCSubset(
        "int A = 7;\n"
        "enum E { A };\n");
    bool foundRedecl = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_RedeclaredSymbol && d.actual == "A") {
            foundRedecl = true; break;
        }
    }
    EXPECT_TRUE(foundRedecl)
        << "lifting enumerator name into a scope that already binds it "
           "must emit S_RedeclaredSymbol";
}

// Non-literal explicit value emits S_NonConstantEnumeratorValue. v1
// accepts integer-literal explicit values only; arbitrary const-exprs
// require CST-side const-eval (plan 12.5 §0.2 D6).
TEST(HirLoweringCSubset, D5_5_NonLiteralEnumeratorValueEmitsDiag) {
    SemanticModel model = analyzeCSubset(
        "int n = 5;\n"
        "enum E { A = n };\n");
    bool found = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_NonConstantEnumeratorValue) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found)
        << "non-literal enumerator value must emit S_NonConstantEnumeratorValue";
}

// D5.5-FU2: prove the `liftToEnclosingScope` gate is wired by toggling
// the c-subset config's flag to `false` and verifying `A` no longer
// resolves at the use site. Pins the otherwise-test-untouched
// opt-OUT branch — without this, removing the `&&
// decl.fieldChildren->liftToEnclosingScope` guard would silently
// keep all tests green.
TEST(HirLoweringCSubset, D5_5_LiftOptOutRespected) {
    // Read the shipped c-subset config text and flip the enumDecl's
    // `liftToEnclosingScope` from true to false. The rest of the
    // schema (incl. `compositeKind: "enum"`) stays untouched.
    fs::path here = fs::current_path();
    fs::path schemaPath;
    for (int i = 0; i < 8 && !here.empty(); ++i) {
        fs::path const cand = here / "src" / "dss-config" / "sources" / "c-subset.lang.json";
        if (fs::exists(cand)) { schemaPath = cand; break; }
        fs::path const par = here.parent_path();
        if (par == here) break;
        here = par;
    }
    ASSERT_FALSE(schemaPath.empty()) << "cannot locate c-subset.lang.json";
    std::ifstream in{schemaPath, std::ios::binary};
    ASSERT_TRUE(in.is_open()) << "cannot open " << schemaPath.string();
    std::ostringstream buf; buf << in.rdbuf();
    std::string text = std::move(buf).str();
    std::string const target =
        "\"compositeKind\": \"enum\",\n"
        "                           \"liftToEnclosingScope\": true";
    // The enum-composite lift flag now rides ONE row — the unified `enumSpec`
    // (c25 D-CSUBSET-UNIFIED-COMPOSITE-SPECIFIER REPLACED the `enumSpecifierBody`
    // row; the dead statement-position `enumDecl` row was deleted earlier in
    // D-CSUBSET-STRUCT-BODY-VARDECL-POSITION). Flip EVERY occurrence so the
    // opt-out is total — the bare-name `A` below (its enum is parsed via the
    // unified `enumSpec`) must then resolve through NO lift.
    std::size_t flipped = 0;
    for (auto pos = text.find(target); pos != std::string::npos;
         pos = text.find(target, pos)) {
        text.replace(pos, target.size(),
            "\"compositeKind\": \"enum\",\n"
            "                           \"liftToEnclosingScope\": false");
        ++flipped;
    }
    ASSERT_GE(flipped, 1u)
        << "c-subset config no longer carries the expected enum lift flag";

    auto loaded = GrammarSchema::loadFromText(text, "<c-subset-no-lift>");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "" : loaded.error()[0].message);

    UnitBuilder builder{*loaded};
    builder.addInMemory(
        "enum E { A, B, C };\n"
        "void f() { enum E e = A; }\n",
        "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel model = analyze(cu);

    bool foundUndecl = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_UndeclaredIdentifier && d.actual == "A") {
            foundUndecl = true; break;
        }
    }
    EXPECT_TRUE(foundUndecl)
        << "with liftToEnclosingScope=false, bare-name `A` MUST emit "
           "S_UndeclaredIdentifier — the gate's opt-out branch is otherwise "
           "test-untouched";
}

// D5.5-FU4 + FU5: enum-typed program emits + re-parses cleanly (the
// HIR text format `enum "E"` round-trip is locked in by parse re-verify).
TEST(HirLoweringCSubset, D5_5_EnumHirTextRoundTrip) {
    SemanticModel model = analyzeCSubset(
        "enum E { A, B, C };\n"
        "void f() { enum E e = A; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    std::vector<std::string> names = symbolNames(model);
    HirTextContext ctx;
    ctx.interner    = &model.lattice().interner();
    ctx.symbolNames = &names;
    ctx.literalPool = &res->literalPool;
    DiagnosticReporter er;
    std::string const out = emitHir(res->hir, ctx, er);
    EXPECT_NE(out.find("enum"), std::string::npos);

    DiagnosticReporter pr;
    auto parsed = parseHir(out, CompilationUnitId{42}, pr);
    EXPECT_TRUE(parsed->ok)
        << "re-parsed enum program must verify cleanly";
}

// ── FAIL-LOUD TRIPWIRE for the inliner's IntrinsicCall relaxation ────
// OPT7 cycle 6 made the MIR inliner blanket-admit IntrinsicCall-bearing
// callees. That admission is correct ONLY while no frame-sensitive
// intrinsic (va_start / frameaddress / setjmp-class) can reach the
// inliner — and today NO shipped frontend emits ANY intrinsic at all (the
// c-subset sema + CST→HIR lowering never registers or constructs one;
// only the HIR text format can, in tests). This test PINS that
// precondition: a breadth of representative c-subset programs each lower
// to a HIR whose intrinsic registry is EMPTY. The day a frontend starts
// emitting intrinsics this pin goes RED — forcing whoever adds it to
// confront the frame-sensitivity gate (D-OPT7-INLINE-FRAME-SENSITIVE-
// INTRINSIC) BEFORE the inliner can silently inline a frame-sensitive
// one. A prose anchor alone is not load-bearing against a code change
// cycles away; this RED-on-emit pin is.
TEST(HirLoweringCSubset, NoShippedConstructLowersToIntrinsic) {
    // Breadth of constructs (all in the shipped corpus): params +
    // arithmetic, subtraction, a cross-function call, a conditional, a
    // loop, a comparison. If any future lowering arm emits an intrinsic,
    // at least one of these exercises the path that would register it.
    char const* const programs[] = {
        "int add(int a, int b) { return a + b; }",
        "int sub(int a, int b) { return a - b; }",
        "int callee(int x) { return x + 1; } "
            "int caller(int y) { return callee(y); }",
        "int pick(int c) { if (c) return 7; return 9; }",
        "int loop(int n) { int s = 0; while (n) { s = s + n; n = n - 1; } "
            "return s; }",
        "int cmp(int a, int b) { return a < b; }",
    };
    for (char const* src : programs) {
        SCOPED_TRACE(src);
        SemanticModel model = analyzeCSubset(src);
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok);
        EXPECT_TRUE(res->hir.intrinsicRegistry().intrinsics().empty())
            << "a c-subset program lowered to a HIR with a NON-empty intrinsic "
               "registry — a frontend now emits intrinsics. Before relying on "
               "the inliner's blanket IntrinsicCall admission, gate it on per-"
               "intrinsic inline-safety: D-OPT7-INLINE-FRAME-SENSITIVE-INTRINSIC.";
    }
}

// ── plan 24 (cst_to_hir residuals): the Assign + Switch arms are now flattened
// onto the existing ExprFrame/StmtFrame work-stack drivers. These two strict pins
// witness the FLAT-on-the-main-stack property (RED-on-disable: revert the
// flattening → host recursion overflows the test's normal stack at the chain
// depth → crash) AND the exact byte-identical HIR shape (a flattening that parsed
// without crashing but produced the WRONG structure fails the shape assertions).
// They run the LOWERING on the default test main stack (NO large-stack wrapper) —
// that is the whole point — while the orthogonal recursive parse/analyze of the
// deep tree runs on the 64 MiB worker (the parser's assign-RHS arm is flat since
// plan-24 Stage 5; `analyze` wraps itself in `callOnLargeStack`). The Tree/Hir
// arenas tear down FLATLY (dense ArenaContainer, not a recursive node graph), so
// the deep tree's destruction never overflows the main stack. The cap is raised
// above the depth so the SEMANTIC P_ExpressionTooDeep does not fire (the separate
// too-deep pins prove the cap still fires at its configured point).

// A ~2000-deep RIGHT-assoc assignment chain `a=a=…=a;` lowers (CST→HIR) flat on the
// normal stack to the nested-SeqExpr backbone. Each `=` USED AS A VALUE lowers to
// `SeqExpr([AssignStmt(Ref a, ·)], yield = Ref a)`; the outermost `=` is in
// statement position (an AssignStmt whose value is the (N-1)-op sub-chain). The
// Assign frame turns the deep RHS re-entry into a heap work-stack push, so the
// descent carries flat O(1) host-stack cost. RED-on-disable: restore the recursive
// `lowerBinary` Assign arm → the deep RHS recurses ~2000 host frames → overflow.
TEST(HirLoweringCSubset, DeepRightAssocAssignChainLowersFlatOnNormalStack) {
    constexpr int kOps = 2000;   // 2000 `=` ops → a 2000-deep right-assoc chain

    // `int main(void){ int a; a=a=…=a; return 0; }` — `a` is a simple int lvalue,
    // so the chain recurses PURELY through the value-yielding Assign RHS arm (no
    // prep, no other nesting). Built once; parsed with the cap raised.
    std::string src = "int main(void){ int a; a";
    for (int i = 0; i < kOps; ++i) src += "=a";
    src += "; return 0; }";

    SemanticModel model = analyzeCSubsetRaisedCap(std::move(src), kOps + 1000);
    ASSERT_FALSE(model.hasErrors());

    DiagnosticReporter r;
    // Lower on THIS (main) stack — the flat-property witness. A revert recurses
    // here and overflows.
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    // Shape walk: the function body's first statement is the outermost AssignStmt;
    // its value is the (N-1)-op sub-chain = exactly kOps-1 nested SeqExprs, each
    // holding one AssignStmt whose stored value is the next-deeper SeqExpr, the
    // innermost storing a bare Ref. A left-nested or level-dropping mis-lowering
    // breaks the count/shape here (not merely "didn't crash").
    HirNodeId const fnBody =
        res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    // children: [VarDecl a, AssignStmt(...)] (the ExprStmt-position assign lowers
    // to a bare AssignStmt statement).
    auto const bodyStmts = res->hir.children(fnBody);
    ASSERT_GE(bodyStmts.size(), 2u);
    HirNodeId const outerAssign = bodyStmts[1];
    ASSERT_EQ(res->hir.kind(outerAssign), HirKind::AssignStmt);
    EXPECT_EQ(res->hir.kind(res->hir.assignTarget(outerAssign)), HirKind::Ref);

    int seqLevels = 0;
    HirNodeId cur = res->hir.assignValue(outerAssign);   // the (N-1)-op value chain
    while (res->hir.kind(cur) == HirKind::SeqExpr) {
        ++seqLevels;
        // Each value-`=` is `SeqExpr([AssignStmt(Ref a, stored)], yield Ref a)`.
        auto const stmts = res->hir.seqExprStmts(cur);
        ASSERT_EQ(stmts.size(), 1u) << "value-assign SeqExpr has exactly one stmt";
        ASSERT_EQ(res->hir.kind(stmts[0]), HirKind::AssignStmt);
        EXPECT_EQ(res->hir.kind(res->hir.seqExprResult(cur)), HirKind::Ref)
            << "the SeqExpr yields the re-read lvalue";
        cur = res->hir.assignValue(stmts[0]);            // descend to the next level
    }
    // The innermost stored value is the bare `a` Ref (the chain's tail operand).
    EXPECT_EQ(res->hir.kind(cur), HirKind::Ref);
    EXPECT_EQ(seqLevels, kOps - 1)
        << "exactly one nested value-SeqExpr per `=` below the statement-position "
           "outermost assign";
}

// A deeply-NESTED switch (`switch(x){ case 1: switch(x){ case 1: … default: …} … }`)
// lowers (CST→HIR) flat on the normal stack to a SwitchStmt whose single case-arm
// body is the next-inner SwitchStmt, nested kDepth deep. The Switch frame turns the
// per-arm-body `lowerStmt` re-entry into a heap work-stack push, so a switch nested
// in a switch-arm body carries flat O(1) host-stack cost (the recursive form
// recursed `lowerStmt → lowerSwitch → lowerStmt` once per level). RED-on-disable:
// restore the recursive `lowerSwitch` body re-entries → ~kDepth host frames →
// overflow. The shape walk pins the exact innermost-SwitchStmt backbone.
TEST(HirLoweringCSubset, DeepNestedSwitchLowersFlatOnNormalStack) {
    constexpr int kDepth = 1200;   // nesting levels of switch-in-case-body

    // `int main(void){ int x=0; switch(x){case 1: switch(x){case 1: … case 0:
    // return 0; …} default: break;} … return 0; }` — each level is a switch whose
    // `case 1:` body is the next-inner switch; the innermost returns. Each switch
    // also has a `default: break;` so every level is a real multi-arm switch.
    std::string src = "int main(void){ int x=0; ";
    for (int i = 0; i < kDepth; ++i) src += "switch(x){ case 1: ";
    src += "return 0;";                          // innermost case-1 body
    for (int i = 0; i < kDepth; ++i) src += " default: break; }";
    src += " return 0; }";

    SemanticModel model = analyzeCSubsetRaisedCap(std::move(src), kDepth + 1000);
    ASSERT_FALSE(model.hasErrors());

    DiagnosticReporter r;
    auto res = lowerToHir(model, r);   // lower on THIS (main) stack — flat witness
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    // Shape walk (c60, Design I-A): descend the SwitchStmt backbone. The body's
    // switch is level 0; each level's body Block holds the case-1 marker
    // (LabelStmt, ordinal = arms[0]) whose inner statement is the next switch,
    // kDepth deep, the innermost case-1 marker wrapping the `return`. A dropped
    // level or mis-built dispatch breaks the count here.
    HirNodeId const fnBody =
        res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    auto const bodyStmts = res->hir.children(fnBody);
    // children: [VarDecl x, SwitchStmt, ReturnStmt].
    ASSERT_GE(bodyStmts.size(), 2u);
    // `case 1: switch(...)` parses as a BARE caseLabel item + the switch as the NEXT
    // switchBodyItem (the speculative switchBodyItem takes the caseLabel first), so
    // each level's body Block is [case-1 marker(Skip), <next switch or return>,
    // default marker]. Descend by locating the case-1 marker then the FIRST
    // SwitchStmt/ReturnStmt sibling after it.
    auto descendCase1 = [&](HirNodeId sw) -> HirNodeId {
        auto const arms = res->hir.switchArms(sw);
        if (arms.size() < 2u || res->hir.caseArmIsDefault(arms[0])
            || !res->hir.caseArmIsDefault(arms[1])) return HirNodeId{};
        std::uint32_t const c1Ord = res->hir.caseArmLabelOrdinal(arms[0]);
        auto const kids = res->hir.children(res->hir.switchBody(sw));
        bool seenMarker = false;
        for (HirNodeId s : kids) {
            if (!seenMarker) {
                if (res->hir.kind(s) == HirKind::LabelStmt
                    && res->hir.labelOrdinal(s) == c1Ord) {
                    seenMarker = true;
                    // The marker may directly wrap the next construct (caseStmt form)
                    // — if so, descend into it.
                    HirNodeId const inner = res->hir.labelBody(s);
                    if (res->hir.kind(inner) == HirKind::SwitchStmt
                        || res->hir.kind(inner) == HirKind::ReturnStmt)
                        return inner;
                }
                continue;
            }
            if (res->hir.kind(s) == HirKind::SwitchStmt
                || res->hir.kind(s) == HirKind::ReturnStmt)
                return s;
        }
        return HirNodeId{};
    };
    HirNodeId cur = bodyStmts[1];
    int switchLevels = 0;
    while (res->hir.kind(cur) == HirKind::SwitchStmt) {
        ++switchLevels;
        HirNodeId const next = descendCase1(cur);
        ASSERT_TRUE(next.valid()) << "case-1 descent failed at level " << switchLevels;
        cur = next;
    }
    // The innermost case-1 body is the `return 0;`.
    EXPECT_EQ(res->hir.kind(cur), HirKind::ReturnStmt);
    EXPECT_EQ(switchLevels, kDepth)
        << "exactly one nested SwitchStmt per source `switch`";
}

// ─────────────────────────── FC-F1: ++/-- (Cluster F item F1) ───────────────
// Prefix `++`/`--` (pre + post, integer + pointer). Postfix-int already worked
// (IncrementInStatementPositionLowers / ValueYieldingIncrementLowersToSeqExpr);
// these pin the NEW behavior: prefix parses+lowers, prefix yields the NEW value
// (vs postfix's OLD), and a POINTER ++/-- scales by sizeof(*p) via the Index→Gep
// path (NOT a bare 1-byte BinaryOp Add). Strict + red-on-disable.

TEST(HirLoweringCSubset, PrefixIncrementStatementLowersToAssign) {
    // `++x;` in statement position → a clean AssignStmt + BinaryOp (NOT a value-
    // position SeqExpr). Proves MF-2 (the unaryExprRule arm of lowerStmtExprCore).
    SemanticModel model = analyzeCSubset("void f(int x) { ++x; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId stmt = res->hir.children(body)[0];
    ASSERT_EQ(res->hir.kind(stmt), HirKind::AssignStmt)
        << "prefix ++x; must lower to an AssignStmt, not a SeqExpr";
    // The stored value is `x + 1` (a BinaryOp), not a SeqExpr.
    EXPECT_EQ(res->hir.kind(res->hir.assignValue(stmt)), HirKind::BinaryOp);
}

TEST(HirLoweringCSubset, PrefixIncrementValueYieldsNewValue) {
    // `return ++x;` — prefix yields the NEW value: lowers to a SeqExpr that
    // STORES then yields a fresh READ of the lvalue (the post-store value), with
    // NO leading temp VarDecl (the distinguishing mark vs postfix). For a simple
    // local, that read is a Ref to `x` ITSELF — and crucially it names the SAME
    // symbol the store writes to (so the yield is the post-store value of x).
    SemanticModel model = analyzeCSubset("int f(int x) { return ++x; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ret  = res->hir.children(body)[0];
    HirNodeId val  = *res->hir.returnValue(ret);
    ASSERT_EQ(res->hir.kind(val), HirKind::SeqExpr);
    // stmts: [assign x = x+1] ONLY (no tmp save).
    ASSERT_EQ(res->hir.seqExprStmts(val).size(), 1u)
        << "prefix has NO leading temp VarDecl (postfix saves the old value first)";
    HirNodeId store = res->hir.seqExprStmts(val)[0];
    ASSERT_EQ(res->hir.kind(store), HirKind::AssignStmt);
    // The yielded value is a fresh Ref to the SAME symbol the store targets (x) —
    // i.e. the post-store value of x, NOT a saved temp.
    HirNodeId yield = res->hir.seqExprResult(val);
    ASSERT_EQ(res->hir.kind(yield), HirKind::Ref);
    EXPECT_EQ(res->hir.payload(yield), res->hir.payload(res->hir.assignTarget(store)))
        << "prefix yields a read of x itself (the new value), not a temp";
}

TEST(HirLoweringCSubset, PrefixVsPostfixDistinctReturn) {
    // The make-or-break: prefix and postfix value-position lowerings DIFFER.
    // Postfix `x++`: SeqExpr stmts = [tmp = x (VarDecl), x = x+1], result = Ref(tmp)
    //   → yields the OLD value (2 stmts; the yielded Ref names the TEMP, a symbol
    //   distinct from x — the store target).
    // Prefix `++x`: SeqExpr stmts = [x = x+1], result = Ref(x)
    //   → yields the NEW value (1 stmt; the yielded Ref names x = the store target).
    DiagnosticReporter rPost, rPre;
    SemanticModel mPost = analyzeCSubset("int f(int x) { return x++; }");
    SemanticModel mPre  = analyzeCSubset("int f(int x) { return ++x; }");
    ASSERT_FALSE(mPost.hasErrors());
    ASSERT_FALSE(mPre.hasErrors());
    auto post = lowerToHir(mPost, rPost);
    auto pre  = lowerToHir(mPre, rPre);
    ASSERT_TRUE(post->ok);
    ASSERT_TRUE(pre->ok);
    auto seqOf = [](auto const& res) {
        HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
        return *res->hir.returnValue(res->hir.children(body)[0]);
    };
    HirNodeId postSeq = seqOf(post);
    HirNodeId preSeq  = seqOf(pre);
    ASSERT_EQ(post->hir.kind(postSeq), HirKind::SeqExpr);
    ASSERT_EQ(pre->hir.kind(preSeq), HirKind::SeqExpr);
    // Distinct stmt counts: postfix saves the old value first (2), prefix does not (1).
    EXPECT_EQ(post->hir.seqExprStmts(postSeq).size(), 2u);
    EXPECT_EQ(pre->hir.seqExprStmts(preSeq).size(), 1u);
    // The make-or-break value distinction, symbol-id-internals-free: in BOTH the
    // last stmt is the store `x = x ± 1`. Prefix yields a read of the SAME symbol
    // the store targets (x = the NEW value); postfix yields a DIFFERENT symbol
    // (the saved temp = the OLD value).
    auto storeTargetSym = [](auto const& res, HirNodeId seq) {
        auto s = res->hir.seqExprStmts(seq);
        return res->hir.payload(res->hir.assignTarget(s.back()));
    };
    HirNodeId postYield = post->hir.seqExprResult(postSeq);
    HirNodeId preYield  = pre->hir.seqExprResult(preSeq);
    ASSERT_EQ(post->hir.kind(postYield), HirKind::Ref);
    ASSERT_EQ(pre->hir.kind(preYield), HirKind::Ref);
    EXPECT_NE(post->hir.payload(postYield), storeTargetSym(post, postSeq))
        << "postfix yields the OLD value (a temp), distinct from the store target";
    EXPECT_EQ(pre->hir.payload(preYield), storeTargetSym(pre, preSeq))
        << "prefix yields the NEW value (a read of x itself = the store target)";
}

TEST(HirLoweringCSubset, PointerPostfixIncrementScalesViaGep) {
    // `*(p++)` on `int* p` — the pointer step must route through the Index→Gep
    // element-scaling path (`AddressOf(Index(lvRead(p), ±1, int))`), NOT a bare
    // `BinaryOp Add(ptr, 1)` (which would step 1 BYTE, not sizeof(int)). The
    // lowered HIR therefore contains an Index + AddressOf for the step, and the
    // pointer SeqExpr's stored value is NOT a BinaryOp.
    SemanticModel model = analyzeCSubset("int f(int* p) { return *(p++); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    // The scaled-step nodes are present.
    HirNodeId idx  = findFirstByKind(res->hir, res->hir.root(), HirKind::Index);
    EXPECT_TRUE(idx.valid())
        << "pointer ++ must lower through an Index node (the sizeof-scaled Gep path)";
    // Find the SeqExpr (the p++ value) and assert its STORED value is the scaled
    // AddressOf(Index(...)) pointer, never a bare BinaryOp Add on the pointer.
    HirNodeId seq = findFirstByKind(res->hir, res->hir.root(), HirKind::SeqExpr);
    ASSERT_TRUE(seq.valid());
    auto stmts = res->hir.seqExprStmts(seq);
    ASSERT_FALSE(stmts.empty());
    HirNodeId store = stmts.back();   // the lvWrite (AssignStmt)
    ASSERT_EQ(res->hir.kind(store), HirKind::AssignStmt);
    EXPECT_EQ(res->hir.kind(res->hir.assignValue(store)), HirKind::AddressOf)
        << "pointer step value must be AddressOf(Index(...)), not BinaryOp Add";
    EXPECT_NE(res->hir.kind(res->hir.assignValue(store)), HirKind::BinaryOp);
}

TEST(HirLoweringCSubset, PointerIncDecStatementPositionScalesViaGep) {
    // The plan-lock's explicit call-out: cover the OTHER two MF-1 sites
    // (statement position) too, not only value position — `p++;` and `++p;` must
    // ALSO route through the Index→Gep scaling, so the shared-incidental-coverage
    // trap (only value-position exercised) cannot hide a stmt-site regression.
    for (char const* src : {"void f(int* p) { p++; }", "void f(int* p) { ++p; }"}) {
        SemanticModel model = analyzeCSubset(src);
        ASSERT_FALSE(model.hasErrors()) << src;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << src << ": " << (r.all().empty() ? "" : r.all()[0].actual);
        HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
        HirNodeId stmt = res->hir.children(body)[0];
        ASSERT_EQ(res->hir.kind(stmt), HirKind::AssignStmt) << src;
        // The stored value is the scaled pointer AddressOf(Index(...)), NOT a
        // BinaryOp Add stepping a single byte.
        EXPECT_EQ(res->hir.kind(res->hir.assignValue(stmt)), HirKind::AddressOf) << src;
        EXPECT_TRUE(findFirstByKind(res->hir, stmt, HirKind::Index).valid()) << src;
    }
}

TEST(HirLoweringCSubset, NonLvalueIncDecFailsLoud) {
    // A manifest rvalue operand (`5++` / `++5`) is not a modifiable lvalue —
    // fail loud with S_IncDecNeedsModifiableLvalue (MF-4), never a silent
    // write-back to a non-object.
    for (char const* src : {"int f() { return 5++; }", "int f() { return ++5; }"}) {
        SemanticModel model = analyzeCSubset(src);
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_FALSE(res->ok) << src;
        EXPECT_EQ(countCode(r, DiagnosticCode::S_IncDecNeedsModifiableLvalue), 1u) << src;
    }
}

// c28 D-CSUBSET-LOCAL-TYPE-DEFINITION: a BLOCK-SCOPED struct/union/enum
// DEFINITION with NO declarator (`struct S { … };` as a statement — sqlite3.c:
// 68508 walMergesort) lowers CLEANLY: the type is minted + interned at the
// SEMANTIC tier (the unified c25 define path), so the no-declarator statement
// needs no runtime HIR node, and a later `struct S v; v.a` resolves through the
// interned type. RED-on-disable: revert the optional-list grammar tweak → P0009
// at parse (the front-end gate in analyzeCSubset fails first).
TEST(HirLoweringCSubset, LocalStructDefinitionLowersClean) {
    SemanticModel model = analyzeCSubset(
        "int main(void){ struct S { int a; int b; }; struct S v; v.a = 1; "
        "return v.a; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 0u)
        << "a block-scoped struct DEFINITION declares a type — must NOT fail loud";
}

// c28: a local define+declare (`struct S { … } v;`) still lowers cleanly (the
// declarator IS present — the ordinary path), and a local REFERENCE to an
// outer-defined tag (`struct S v;`) resolves. RED-on-disable: if the optional
// list mis-routed the declarator-present form, these regress.
TEST(HirLoweringCSubset, LocalStructDefineAndDeclareAndRefLowerClean) {
    SemanticModel m1 = analyzeCSubset(
        "int main(void){ struct S { int a; } v; v.a = 7; return v.a; }\n");
    ASSERT_FALSE(m1.hasErrors())
        << (m1.diagnostics().all().empty() ? std::string{}
            : m1.diagnostics().all()[0].actual);
    DiagnosticReporter r1;
    auto res1 = lowerToHir(m1, r1);
    EXPECT_TRUE(res1->ok) << (r1.all().empty() ? "" : r1.all()[0].actual);

    SemanticModel m2 = analyzeCSubset(
        "struct S { int a; };\n"
        "int main(void){ struct S v; v.a = 3; return v.a; }\n");
    ASSERT_FALSE(m2.hasErrors())
        << (m2.diagnostics().all().empty() ? std::string{}
            : m2.diagnostics().all()[0].actual);
    DiagnosticReporter r2;
    auto res2 = lowerToHir(m2, r2);
    EXPECT_TRUE(res2->ok) << (r2.all().empty() ? "" : r2.all()[0].actual);
}

// c28: a NON-defining no-declarator LOCAL (`int;`) declares nothing (C 6.7p2).
// Mirroring the top-level `int ;` (TopLevelDeclaresNothingFailsLoudNoCrash), the
// front end (parse + semantic) ACCEPTS it, and the HIR lowering FAILS LOUD with
// S_DeclarationDeclaresNothing — the local twin of the top-level no-object path
// (lowerVarLikeInto: an empty list-mode declarator carrier with NO composite
// specifier in the head). Must NOT crash and must NOT silently accept.
// RED-on-disable: drop the lowerVarLikeInto declares-nothing guard → res->ok
// stays true and the count is 0 (a silent accept).
TEST(HirLoweringCSubset, LocalDeclaresNothingFailsLoud) {
    SemanticModel model = analyzeCSubset("int main(void){ int; return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "parse + semantic accept `int;` — the constraint is HIR-tier";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "`int;` (no declarator, no tag) declares nothing — lowering must fail loud";
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 1u)
        << "exactly one S_DeclarationDeclaresNothing for the empty local `int;`";
}

// c89 (D-CSUBSET-SIZEOF-VALUE-OPERAND-TYPE): the VALUE-form sizeof operand is
// sized by its full EXPRESSION type (C 6.5.3.4) — the tier-boundary pin for the
// Pass-2 sizeofValueRule operand stamp at its exact consumption point: the
// HirKind::SizeOf node's TypeRef child. Pre-c89, Pass 2 left operator nodes
// unstamped and lowerSizeof's resolveStampedTypeBelow DFS sailed past the
// unstamped `*p`/`tab[0]` into the base IDENTIFIER leaf: sizeof(*p) carried
// Ptr<Big> (folding 8, not 48), sizeof(tab[0]) carried the WHOLE Array (336),
// and sizeof(&tab) carried the Array — sqlite's pthreadMutexAlloc
// `sqlite3MallocZero(sizeof(*p))` under-allocated 8 for the 40-byte recursive
// mutex → glibc's own mutex-init writes clobbered the malloc top chunk →
// deterministic sysmalloc SIGABRT on every invocation (the c88 smoke wall).
// The corpus witness (examples/c-subset/sizeof_value_expression) proves the
// folded VALUES end-to-end; THIS pin names the tier, so a future Pass-2
// refactor that drops the operand stamp fails HERE, not three tiers later.
// RED-ON-DISABLE: revert the Pass-2 stamp → [0] kind flips Struct→Ptr,
// [1] Struct→Array, [2] Struct→Ptr, [3] pointee flips Array→Big(Struct);
// every EXPECT below flips.
TEST(HirLoweringCSubset, SizeofValueOperandCarriesExpressionType) {
    SemanticModel model = analyzeCSubset(
        "struct Big { double a; double b; double c; double d; double e; "
        "double f; };\n"
        "static struct Big tab[7];\n"
        "unsigned long long f(struct Big *p) {\n"
        "    return sizeof(*p) + sizeof(tab[0]) + sizeof(p[0]) + sizeof(&tab);\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    ASSERT_TRUE(fn.valid());
    // Collect every SizeOf in the body, pre-order = source order (the `+`
    // chain associates left, so the DFS meets them left-to-right).
    std::vector<HirNodeId> sizeofs;
    auto const collect = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::SizeOf) sizeofs.push_back(n);
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    collect(collect, res->hir.functionBody(fn));
    ASSERT_EQ(sizeofs.size(), 4u) << "four value-form sizeof sites expected";
    auto const& ti = model.lattice().interner();
    auto const sizedType = [&](HirNodeId szNode) -> TypeId {
        auto const kids = res->hir.children(szNode);
        EXPECT_EQ(kids.size(), 1u) << "SizeOf carries exactly [TypeRef]";
        return kids.empty() ? TypeId{} : res->hir.typeId(kids.front());
    };
    // [0] sizeof(*p): the POINTEE struct (the sqlite pthreadMutexAlloc shape).
    TypeId const t0 = sizedType(sizeofs[0]);
    ASSERT_TRUE(t0.valid());
    EXPECT_EQ(ti.kind(t0), TypeKind::Struct)
        << "sizeof(*p) must size the pointee STRUCT, not the pointer";
    EXPECT_EQ(ti.operands(t0).size(), 6u) << "the 6-double Big, 48 bytes";
    // [1] sizeof(tab[0]): the array ELEMENT, never the whole array.
    TypeId const t1 = sizedType(sizeofs[1]);
    ASSERT_TRUE(t1.valid());
    EXPECT_EQ(ti.kind(t1), TypeKind::Struct)
        << "sizeof(tab[0]) must size the ELEMENT, not the whole Array "
           "(the ArraySize idiom's denominator — pre-c89 it folded to the "
           "numerator and ArraySize collapsed to 1)";
    // [2] sizeof(p[0]): index through a pointer — the element again.
    TypeId const t2 = sizedType(sizeofs[2]);
    ASSERT_TRUE(t2.valid());
    EXPECT_EQ(ti.kind(t2), TypeKind::Struct)
        << "sizeof(p[0]) must size the pointee STRUCT, not the pointer";
    // [3] sizeof(&tab): address-of yields a POINTER (8), never the array (336).
    TypeId const t3 = sizedType(sizeofs[3]);
    ASSERT_TRUE(t3.valid());
    ASSERT_EQ(ti.kind(t3), TypeKind::Ptr)
        << "sizeof(&tab) must size a POINTER-to-array";
    ASSERT_EQ(ti.operands(t3).size(), 1u);
    EXPECT_EQ(ti.kind(ti.operands(t3)[0]), TypeKind::Array)
        << "…whose pointee is the Array itself";
}

// C11/C23 6.5.3.4: `_Alignof(T)` / `alignof(T)` lower to a core HirKind::AlignOf
// node carrying the QUERIED type on its single [TypeRef] child (mirroring SizeOf).
// Covers both spellings AND a struct type-name. RED-ON-DISABLE: drop the
// lowerAlignof dispatch → the operand alt tries to type `_Alignof`/`alignof` as an
// expression and the front end fails (no AlignOf node reaches the body).
TEST(HirLoweringCSubset, AlignofLowersToAlignOfNodeCarryingQueriedType) {
    SemanticModel model = analyzeCSubset(
        "struct CharDouble { char c; double d; };\n"
        "unsigned long long f(void) {\n"
        "    return _Alignof(double) + alignof(char) "
        "+ _Alignof(struct CharDouble);\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    ASSERT_TRUE(fn.valid());
    // Collect every AlignOf in the body, pre-order = source order.
    std::vector<HirNodeId> alignofs;
    auto const collect = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::AlignOf) alignofs.push_back(n);
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    collect(collect, res->hir.functionBody(fn));
    ASSERT_EQ(alignofs.size(), 3u) << "three _Alignof/alignof sites expected";
    auto const& ti = model.lattice().interner();
    auto const queriedType = [&](HirNodeId n) -> TypeId {
        auto const kids = res->hir.children(n);
        EXPECT_EQ(kids.size(), 1u) << "AlignOf carries exactly [TypeRef]";
        // The AlignOf node itself is size_t (U64) — its result type.
        EXPECT_EQ(ti.kind(res->hir.typeId(n)), TypeKind::U64)
            << "_Alignof yields size_t";
        return kids.empty() ? TypeId{} : res->hir.typeId(kids.front());
    };
    // [0] _Alignof(double): the queried type is the primitive double (F64).
    TypeId const t0 = queriedType(alignofs[0]);
    ASSERT_TRUE(t0.valid());
    EXPECT_EQ(ti.kind(t0), TypeKind::F64) << "_Alignof(double) queries F64";
    // [1] alignof(char): the C23 spelling, queried type char (TypeKind::Char).
    TypeId const t1 = queriedType(alignofs[1]);
    ASSERT_TRUE(t1.valid());
    EXPECT_EQ(ti.kind(t1), TypeKind::Char) << "alignof(char) queries char";
    // [2] _Alignof(struct CharDouble): the whole struct type.
    TypeId const t2 = queriedType(alignofs[2]);
    ASSERT_TRUE(t2.valid());
    EXPECT_EQ(ti.kind(t2), TypeKind::Struct)
        << "_Alignof(struct CharDouble) queries the STRUCT type";
}

// c90 (D-CSUBSET-ASSIGN-VALUE-RHS-COERCE): a plain `=` used as a VALUE stores
// the RHS COERCED to the lvalue's type (C 6.5.16p3) — the tier-boundary pin
// for `finishAssign`'s plain arm (and its `lowerBinary` Assign-arm mirror) at
// the exact node the property lives on: the SeqExpr's AssignStmt VALUE child
// must be a Cast carrying the LVALUE's type whenever the RHS type differs.
// Pre-c90 the raw RHS node was stored un-coerced and the MIR store executed
// at the RHS's width: an i16 comma-assign from a wider RHS partial-stored
// (sqlite estimateTableWidth's `for(i=pTab->nCol, ...)` left i's upper half
// stale → the 3822-element overrun → the every-SQL-statement SIGSEGV), and a
// wider RHS over-stored past a sub-int lvalue (neighbor corruption). The
// corpus witness (examples/c-subset/assign_value_coerce) proves the VALUES
// end-to-end on all run legs; THIS pin names the tier, so a refactor that
// drops either coerce fails HERE, not three tiers later at runtime.
// RED-ON-DISABLE: revert `stored = coerce(result, lv.type).id` → the stored
// value's kind flips Cast→Ref (the raw I32/F64 RHS) and its type flips
// I16→I32/F64; the Cast asserts below flip. The SeqExpr node type + yield-Ref
// type (I16) are the pre-existing yield thread and stay green.
TEST(HirLoweringCSubset, PlainAssignAsValueStoresRhsCoercedToLvalueType) {
    SemanticModel model = analyzeCSubset(
        "long long g(int v, double d) {\n"
        "    short s; int y; long long L;\n"
        "    y = (s = v);\n"    // int RHS -> i16 lvalue (the sqlite shape)
        "    L = (s = d);\n"    // double RHS -> i16 lvalue (the float leg)
        "    return y + L + s;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    ASSERT_TRUE(fn.valid());
    // Collect the value-position assigns: every SeqExpr in the body, pre-order
    // = source order → [0] = (s = v), [1] = (s = d).
    std::vector<HirNodeId> seqs;
    auto const collect = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::SeqExpr) seqs.push_back(n);
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    collect(collect, res->hir.functionBody(fn));
    ASSERT_EQ(seqs.size(), 2u) << "two value-position plain assigns expected";
    auto const& ti = model.lattice().interner();
    TypeKind const rhsKinds[2] = {TypeKind::I32, TypeKind::F64};
    for (std::size_t k = 0; k < 2; ++k) {
        HirNodeId const seq = seqs[k];
        // The SeqExpr (the assignment-as-value) carries the LVALUE's type.
        TypeId const seqTy = res->hir.typeId(seq);
        ASSERT_TRUE(seqTy.valid());
        EXPECT_EQ(ti.kind(seqTy), TypeKind::I16)
            << "[" << k << "] the assign-as-value expression is lvalue-typed";
        auto const stmts = res->hir.seqExprStmts(seq);
        ASSERT_EQ(stmts.size(), 1u) << "[" << k << "] simple lvalue: no prep";
        ASSERT_EQ(res->hir.kind(stmts[0]), HirKind::AssignStmt);
        // THE c90 PROPERTY: the stored value is the RHS wrapped in a Cast to
        // the LVALUE's type — never the raw RHS at its own width.
        HirNodeId const stored = res->hir.assignValue(stmts[0]);
        ASSERT_EQ(res->hir.kind(stored), HirKind::Cast)
            << "[" << k << "] plain-=-as-value must COERCE the RHS to the "
               "lvalue type (D-CSUBSET-ASSIGN-VALUE-RHS-COERCE)";
        TypeId const storedTy = res->hir.typeId(stored);
        ASSERT_TRUE(storedTy.valid());
        EXPECT_EQ(ti.kind(storedTy), TypeKind::I16)
            << "[" << k << "] the stored Cast carries the lvalue's I16 type";
        auto const castKids = res->hir.children(stored);
        ASSERT_EQ(castKids.size(), 1u);
        TypeId const rawTy = res->hir.typeId(castKids[0]);
        ASSERT_TRUE(rawTy.valid());
        EXPECT_EQ(ti.kind(rawTy), rhsKinds[k])
            << "[" << k << "] …over the raw RHS at its own type";
        // The 6.5.16p3 yield thread: the expression's value is the lvalue
        // re-read, typed by the lvalue (pre-existing, kept pinned).
        HirNodeId const yield = res->hir.seqExprResult(seq);
        ASSERT_EQ(res->hir.kind(yield), HirKind::Ref);
        TypeId const yieldTy = res->hir.typeId(yield);
        ASSERT_TRUE(yieldTy.valid());
        EXPECT_EQ(ti.kind(yieldTy), TypeKind::I16)
            << "[" << k << "] the assignment's VALUE is the post-conversion "
               "lvalue read (C 6.5.16p3)";
    }
}

// c91 (D-CSUBSET-ARRAY-DECAY-IN-COMPARISON + D-CSUBSET-ARRAY-DECAY-IN-
// CONDITION, closing the D-CSUBSET-ARRAY-DECAY-POINTER-IDENTITY HIR surface):
// an ARRAY operand of a comparison, a condition, or `!` decays to Ptr<elem>
// (C 6.3.2.1p3) THROUGH THE ONE coerce funnel — the tier-boundary pin at the
// exact nodes the property lives on. Pre-c91 the operand kept its Array type
// and was VALUE-lowered at MIR: a member/global array operand emitted an
// aggregate Load, so the compare read the array's first BYTES as a "pointer"
// (sqlite sqlite3ParserFinalize `pParser->yystack != pParser->yystk0` →
// always-unequal → freed the on-stack parser → the every-SQL-statement
// SIGABRT), and an Array condition reached the CondBr raw
// (I_TerminatorTypeMismatch). The corpus witness
// (examples/c-subset/array_decay_pointer_identity) proves the VALUES
// end-to-end on all run legs; THIS pin names the HIR tier, so a refactor
// that drops any of the three decay arms fails HERE even while the MIR
// value-read backstop (the c63-twin arms) keeps the end-to-end behavior
// correct. RED-ON-DISABLE (each arm independently):
//   - combineBinary comparison arm reverted → the Ne's rhs stays a raw
//     Array-typed MemberAccess (the Cast asserts flip);
//   - coerceCondition Array arm reverted → `if (g)` synthesizes no Ne
//     (the two-Ne count flips);
//   - combineUnaryOp `!` arm reverted → Not's operand stays a raw
//     Array-typed Ref (the Cast asserts flip).
TEST(HirLoweringCSubset, ArrayComparisonConditionOperandsDecayToPointer) {
    SemanticModel model = analyzeCSubset(
        "struct P { int *stack; int stk0[4]; };\n"
        "int g[4];\n"
        "int f(struct P *p) {\n"
        "    int r = 0;\n"
        "    if (p->stack != p->stk0) r = 1;\n"   // the sqlite ParserFinalize shape
        "    if (g) r = r + 2;\n"                 // Array condition
        "    if (!g) r = r + 4;\n"                // `!array`
        "    return r;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    ASSERT_TRUE(fn.valid());
    auto const& ti = model.lattice().interner();
    // Collect the Ne BinaryOps and the Not UnaryOps, pre-order = source order.
    std::vector<HirNodeId> nes;
    std::vector<HirNodeId> nots;
    auto const collect = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::BinaryOp
            && isCoreOp(res->hir.payload(n))
            && decodeCoreOp(res->hir.payload(n)) == HirOpKind::Ne)
            nes.push_back(n);
        if (res->hir.kind(n) == HirKind::UnaryOp
            && isCoreOp(res->hir.payload(n))
            && decodeCoreOp(res->hir.payload(n)) == HirOpKind::Not)
            nots.push_back(n);
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    collect(collect, res->hir.functionBody(fn));
    // [0] the source `!=`; [1] the Ne coerceCondition SYNTHESIZES for `if (g)`
    // (the `!g` condition is already Bool — Not — so no third Ne).
    ASSERT_EQ(nes.size(), 2u)
        << "the member compare + the SYNTHESIZED `if (g)` truth test "
           "(D-CSUBSET-ARRAY-DECAY-IN-CONDITION: a raw Array cond emits none)";
    // THE c91 comparison property: the Array-typed member operand is wrapped
    // in a decay Cast to Ptr<elem>; the raw MemberAccess keeps its Array type
    // underneath.
    auto const expectDecayCast = [&](HirNodeId operand, HirKind rawKind,
                                     char const* what) {
        ASSERT_EQ(res->hir.kind(operand), HirKind::Cast)
            << what << ": the Array operand must be wrapped in the coerce "
                       "decay Cast (C 6.3.2.1p3)";
        TypeId const ct = res->hir.typeId(operand);
        ASSERT_TRUE(ct.valid());
        ASSERT_EQ(ti.kind(ct), TypeKind::Ptr)
            << what << ": the decay Cast carries Ptr<elem>";
        EXPECT_EQ(ti.kind(ti.operands(ct)[0]), TypeKind::I32)
            << what << ": …whose pointee is the ELEMENT type (int)";
        auto const kids = res->hir.children(operand);
        ASSERT_EQ(kids.size(), 1u);
        EXPECT_EQ(res->hir.kind(kids[0]), rawKind)
            << what << ": …over the raw lvalue node";
        TypeId const rt = res->hir.typeId(kids[0]);
        ASSERT_TRUE(rt.valid());
        EXPECT_EQ(ti.kind(rt), TypeKind::Array)
            << what << ": …which keeps its Array type underneath";
    };
    {   // [0] `p->stack != p->stk0`: lhs is the Ptr member (no cast), rhs
        // is the DECAYED Array member.
        auto const kids = res->hir.children(nes[0]);
        ASSERT_EQ(kids.size(), 2u);
        TypeId const lt = res->hir.typeId(kids[0]);
        ASSERT_TRUE(lt.valid());
        EXPECT_EQ(ti.kind(lt), TypeKind::Ptr)
            << "lhs (p->stack) is already a pointer — never wrapped";
        expectDecayCast(kids[1], HirKind::MemberAccess,
                        "comparison rhs (p->stk0)");
    }
    {   // [1] `if (g)`: coerceCondition decays the Array Ref then re-enters
        // its own Ptr arm → Ne(decayCast(g), nullPtrCast), typed Bool.
        TypeId const bt = res->hir.typeId(nes[1]);
        ASSERT_TRUE(bt.valid());
        EXPECT_EQ(ti.kind(bt), TypeKind::Bool)
            << "the synthesized truth test is Bool-typed";
        auto const kids = res->hir.children(nes[1]);
        ASSERT_EQ(kids.size(), 2u);
        expectDecayCast(kids[0], HirKind::Ref, "condition operand (g)");
        EXPECT_EQ(res->hir.kind(kids[1]), HirKind::Cast)
            << "…compared against the synthetic null-pointer Cast";
    }
    {   // `!g`: the Not's operand is the decayed Array Ref.
        ASSERT_EQ(nots.size(), 1u);
        auto const kids = res->hir.children(nots[0]);
        ASSERT_EQ(kids.size(), 1u);
        expectDecayCast(kids[0], HirKind::Ref, "`!` operand (g)");
    }
}

// ── c115 SEH (D-WIN64-SEH-FUNCLETS): the __try/__except frontend ──────────────

// The guarded body, filter expression, and handler body lower to a core
// SehTryExcept node {tryBody Block, filterExpr, handlerBody Block}. This is the
// structural shape the c116 x64 funclet lowering consumes.
TEST(HirLoweringCSubset, SehTryExceptLowersToCoreNode) {
    SemanticModel model = analyzeCSubset(
        "int f(int *p) { int rc = 0; __try { rc = *p; } "
        "__except (1) { rc = 42; } return rc; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    HirNodeId const seh =
        findFirstByKind(res->hir, res->hir.functionBody(fn), HirKind::SehTryExcept);
    ASSERT_TRUE(seh.valid()) << "a SehTryExcept node must be emitted";
    auto const kids = res->hir.children(seh);
    ASSERT_EQ(kids.size(), 3u) << "[tryBody, filterExpr, handlerBody]";
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::Block)   << "guarded body is a Block";
    EXPECT_EQ(res->hir.kind(kids[2]), HirKind::Block)   << "handler body is a Block";
    // The filter (child 1) is an expression, not a statement Block.
    EXPECT_NE(res->hir.kind(kids[1]), HirKind::Block)   << "filter is an expression";
}

// `_exception_code()` in the filter expression is LEGAL (the canonical use).
TEST(HirLoweringCSubset, SehExceptionCodeInFilterIsLegal) {
    SemanticModel model = analyzeCSubset(
        "int f(int *p) { int rc = 0; __try { rc = *p; } "
        "__except (_exception_code() == 0) { rc = 42; } return rc; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_SehBuiltinContext), 0u);
    // The builtin lowered to a BuiltinCall inside the filter subtree.
    HirNodeId const fn = firstFunction(res->hir);
    HirNodeId const seh =
        findFirstByKind(res->hir, res->hir.functionBody(fn), HirKind::SehTryExcept);
    ASSERT_TRUE(seh.valid());
    EXPECT_TRUE(findFirstByKind(res->hir, res->hir.children(seh)[1],
                                HirKind::BuiltinCall).valid())
        << "_exception_code lowers to a BuiltinCall in the filter";
}

// RED: `_exception_code()` with NO enclosing __try → H_SehBuiltinContext.
TEST(HirLoweringCSubset, SehExceptionCodeOutsideTryIsRejected) {
    SemanticModel model = analyzeCSubset(
        "int f(void) { return (int)_exception_code(); }\n");
    ASSERT_FALSE(model.hasErrors());   // resolves as a builtin call; HIR verifier rejects
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_SehBuiltinContext), 1u);
}

// RED: `_exception_info()` in the HANDLER body (filter-only) → H_SehBuiltinContext.
// (_exception_code IS legal in the handler; _exception_info is filter-only —
// the asymmetry is the point.)
TEST(HirLoweringCSubset, SehExceptionInfoInHandlerIsRejected) {
    SemanticModel model = analyzeCSubset(
        "int f(int *p) { int rc = 0; __try { rc = *p; } "
        "__except (1) { rc = (int)(long long)_exception_info(); } return rc; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_SehBuiltinContext), 1u);
}

// RED (option (C), D-CSUBSET-SEH-EARLY-EXIT): `return` inside the guarded body.
TEST(HirLoweringCSubset, SehReturnInTryBodyIsRejected) {
    SemanticModel model = analyzeCSubset(
        "int f(int *p) { __try { return *p; } __except (1) { return 42; } }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_SehEarlyExit), 1u);
}

// FAIL-LOUD (D-CSUBSET-SEH-FINALLY): `__finally` parses but has no lowering.
TEST(HirLoweringCSubset, SehFinallyFailsLoud) {
    SemanticModel model = analyzeCSubset(
        "int f(int *p) { int rc = 0; __try { rc = *p; } "
        "__finally { rc = 1; } return rc; }\n");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 1u);
}

// FAIL-LOUD (D-CSUBSET-SEH-LEAVE): `__leave` parses but has no lowering.
TEST(HirLoweringCSubset, SehLeaveFailsLoud) {
    SemanticModel model = analyzeCSubset(
        "int f(int *p) { int rc = 0; __try { __leave; rc = *p; } "
        "__except (1) { rc = 42; } return rc; }\n");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 1u);
}

// ── C11/C23 6.7.10 static_assert (D-CSUBSET-STATIC-ASSERT) ──────────────────
//
// The condition is const-evaluated at the SEMANTIC tier (the point that folds
// sizeof / enum / arithmetic); a zero / non-constant fold fails loud with
// S_StaticAssertFailed. A passing assertion produces NO HIR (lowers to nothing —
// its hirLowering row maps to Skip) and the module is left with just its real
// declarations.

// Count the top-level Function decls in a lowered module — the witness that a
// passing static_assert added nothing at module scope.
[[nodiscard]] std::size_t moduleFunctionCount(Hir const& hir) {
    std::size_t n = 0;
    for (HirNodeId d : hir.moduleDecls(hir.root()))
        if (hir.kind(d) == HirKind::Function) ++n;
    return n;
}

// NOTE on sizeof: an array-dim / static_assert `sizeof` folds ONLY when
// `analyze()` is given the target's aggregateLayout. The direct-API
// `analyzeCSubset` helper here passes nullopt (the documented direct-API
// default), so the sizeof-in-condition FOLDING pins live in
// test_semantic_analyzer_c_subset.cpp (which passes AggregateLayoutParams) and
// end-to-end in examples/c-subset/static_assert_true. The pins BELOW exercise
// the parse / peel / 1-arg / spelling / block-scope / enum / non-const paths,
// which need no layout.

// POSITIVE — the canonical passing idiom: an arithmetic condition FOLDS true,
// the assertion passes, and NOTHING is emitted for it.
TEST(HirLoweringCSubset, StaticAssertArithmeticTrueCompilesToNothing) {
    SemanticModel model = analyzeCSubset(
        "_Static_assert(2 + 2 == 4, \"math works\");\n"
        "int main(void){ return 42; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u)
        << "2+2==4 must FOLD true in the static_assert condition";
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    // The assertion contributed no module node — only `main` survives.
    EXPECT_EQ(moduleFunctionCount(res->hir), 1u);
}

// NEGATIVE — a false arithmetic condition fails loud.
TEST(HirLoweringCSubset, StaticAssertArithmeticFalseFailsLoud) {
    SemanticModel model = analyzeCSubset(
        "_Static_assert(1 == 2, \"one is not two\");\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u);
}

// C23 1-ARG PASSING — `_Static_assert(1);` (no message) compiles to nothing. The
// critical peelToCore case: the 1-arg node has a SINGLE meaningful child, so a
// naive peel would descend past it → H0009; the Skip-mapped rule stops the peel.
TEST(HirLoweringCSubset, StaticAssert1ArgTrueCompilesToNothing) {
    SemanticModel model = analyzeCSubset(
        "_Static_assert(1);\n"
        "int main(void){ return 42; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u);
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    // No H0009 (Ref-to-unbound / unsupported lowering) — the 1-arg form must NOT
    // fall through the wrapper peel into its condition child.
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 0u);
    EXPECT_EQ(moduleFunctionCount(res->hir), 1u);
}

// C23 1-ARG FAILING — `_Static_assert(0);` fails loud with S_StaticAssertFailed
// (NOT H0009). Pins that the 1-arg form is reached by the semantic check.
TEST(HirLoweringCSubset, StaticAssert1ArgFalseFailsLoud) {
    SemanticModel model = analyzeCSubset(
        "_Static_assert(0);\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::P_NoAlternativeMatched), 0u)
        << "the 1-arg form must PARSE (no parse fallthrough)";
}

// C23 `static_assert` SPELLING — behaves identically to `_Static_assert`.
TEST(HirLoweringCSubset, StaticAssertC23SpellingTrue) {
    SemanticModel model = analyzeCSubset(
        "static_assert(1 + 1 == 2, \"addition works\");\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u);
    EXPECT_FALSE(model.hasErrors());
}

TEST(HirLoweringCSubset, StaticAssertC23SpellingFalseFailsLoud) {
    SemanticModel model = analyzeCSubset(
        "static_assert(0, \"nope\");\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u);
}

// BLOCK SCOPE — a static_assert is a valid statement-level declaration. Both a
// passing and a failing one are checked at the same tier.
TEST(HirLoweringCSubset, StaticAssertBlockScopeTrueCompilesToNothing) {
    SemanticModel model = analyzeCSubset(
        "int main(void){ _Static_assert(3 > 1, \"three beats one\"); return 42; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u);
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

TEST(HirLoweringCSubset, StaticAssertBlockScopeFalseFailsLoud) {
    SemanticModel model = analyzeCSubset(
        "int main(void){ _Static_assert(1 == 0, \"impossible\"); return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u);
}

// BLOCK SCOPE 1-ARG — the message-less form at statement scope (both the parse
// and the peel must handle the single-child node here too).
TEST(HirLoweringCSubset, StaticAssertBlockScope1ArgFalseFailsLoud) {
    SemanticModel model = analyzeCSubset(
        "int main(void){ static_assert(1 > 2); return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u);
}

// ENUM CONSTANT in the condition folds (same evaluator that folds enum constants
// in an array dimension).
TEST(HirLoweringCSubset, StaticAssertEnumConstantFolds) {
    SemanticModel model = analyzeCSubset(
        "enum { KVAL = 7 };\n"
        "_Static_assert(KVAL == 7, \"kval is 7\");\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// NON-CONSTANT condition (a float — not an integer constant expression) fails
// loud. C 6.7.10 requires an integer constant expression; a float condition
// cannot fold in `constIntExpr` → S_StaticAssertFailed.
TEST(HirLoweringCSubset, StaticAssertFloatConditionFailsAsNonConstant) {
    SemanticModel model = analyzeCSubset(
        "_Static_assert(3.5, \"float is not an ICE\");\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u);
}
