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

// D5.2 cycle 1: adding `Identifier` to `typeBase` lets typedef'd / struct-tag
// names work bare in type position at top level. The engine's `resolveTypeNode`
// already resolved Identifier-in-type-position via the SE5 alias path
// (Type-kind symbol → its `.type`); this cycle's contribution is the schema
// change that lets the parser accept the form. Block-scope alias (`{ Foo x; }`)
// is intentionally deferred — it collides with `exprStmt` at the statement
// alt and needs speculative-alt support (later cycle).
//
// **Known C divergence**: c-subset has a single identifier namespace (no
// separate "tag namespace"), and `resolveTypeNode`'s alias lookup doesn't
// distinguish typedef-minted Type symbols from struct-tag Type symbols.
// Consequence: after `struct Foo { ... };` alone (no typedef), `Foo x;` ALSO
// lowers cleanly — i.e. every struct tag is implicitly usable as a bare type
// name. Real C requires `struct Foo` or an explicit typedef. The two tests
// below pin both shapes honestly.
TEST(HirLoweringCSubset, TypedefStructAliasAtTopLevel) {
    // The alias name must differ from the struct tag because the single
    // identifier namespace rejects same-name redeclaration. See the negative
    // test `TypedefSameNameAsTagRedeclaresInSingleNamespace` below.
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

// D5.2 cycle 1 (review-fix): pin the bare-struct-tag-as-type-name behavior
// that falls out of the schema change. Without a typedef, `Foo x;` ALSO
// works — the resolveTypeNode SE5 alias path doesn't distinguish struct
// tags from typedef'd Type symbols. Documented as a known C divergence; this
// test pins it so a future cycle that separates the namespaces fails loud.
TEST(HirLoweringCSubset, BareStructTagUsableAsTypeName) {
    SemanticModel model = analyzeCSubset(
        "struct Foo { int x; };\n"
        "Foo bare;\n");                  // no typedef
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

// D5.2 cycle 1 (review-fix): pin the negative — same-name typedef of a
// struct tag fires S_RedeclaredSymbol (single namespace). Catches any
// future cycle that mistakenly relaxes the same-scope dup check.
TEST(HirLoweringCSubset, TypedefSameNameAsTagRedeclaresInSingleNamespace) {
    SemanticModel model = analyzeCSubset(
        "struct Foo { int x; };\n"
        "typedef struct Foo Foo;\n");    // same name as the tag — collides
    EXPECT_TRUE(model.hasErrors());
    bool sawRedecl = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_RedeclaredSymbol) { sawRedecl = true; break; }
    }
    EXPECT_TRUE(sawRedecl)
        << "expected S_RedeclaredSymbol on `typedef struct Foo Foo;`";
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
    // `cond ? a : b` lowers to a HIR Ternary [cond, then, else]. With HR's
    // implicit-coercion pass, the cond is wrapped in `Cast(_, Bool)` when
    // its source type is non-Bool — matches the CondBr-expects-Bool
    // discipline at the MIR boundary.
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
    // cond is now `Cast(Ref(x), Bool)` after the implicit-coercion pass.
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::Cast);
    auto castKids = res->hir.children(kids[0]);
    ASSERT_EQ(castKids.size(), 1u);
    EXPECT_EQ(res->hir.kind(castKids[0]), HirKind::Ref);   // cond's operand: x
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::Literal);   // then: 1
    EXPECT_EQ(res->hir.kind(kids[2]), HirKind::Literal);   // else: 2
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
// rather than the lexical scope Pass 2 stamps).
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
    EXPECT_EQ(res->hir.children(init).size(), 2u);
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
