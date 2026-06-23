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
#include "hir/const_eval.hpp"
#include "hir/hir.hpp"
#include "hir/hir_intrinsic_registry.hpp"
#include "hir/hir_text.hpp"
#include "hir/lowering/cst_to_hir.hpp"

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

TEST(HirLoweringCSubset, SwitchGroupsFlatCases) {
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

// SCALAR compound literals — `(int){42}`, valid C 6.5.2.5p9 — now
// resolve their TYPE (the semantic stamping admits them) but the HIR
// brace-init lowering is aggregate-only by design: the scalar shape
// stays a deliberate FAIL-LOUD ("brace-init target type must be
// struct, union, or array"), never a silent misparse. Lifting the
// scalar restriction is brace-init-lowering work, distinct from this
// anchor's typedef-admission scope.
TEST(HirLoweringCSubset, ScalarCompoundLiteralStaysAggregateOnlyFailLoud) {
    SemanticModel model = analyzeCSubset(
        "int main() { int x = (int){42}; return x; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "the semantic tier must admit + type the scalar compound "
           "literal (the stamp resolves)";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "scalar compound literals are aggregate-gated at the HIR "
           "brace-init lowering today — this must stay LOUD until the "
           "scalar arm lands";
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
    // The enum-composite lift flag now rides ONE row — the `enumSpecifierBody`
    // (the dead statement-position `enumDecl` row was deleted in the struct-head
    // closing cycle, D-CSUBSET-STRUCT-BODY-VARDECL-POSITION). Flip EVERY
    // occurrence so the opt-out is total — the bare-name `A` below (its enum is
    // parsed via `enumSpecifierBody`) must then resolve through NO lift.
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
