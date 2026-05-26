// SE2 acceptance: c-subset language end-to-end via the same
// SchemaDrivenSemantics engine — proves zero per-language C++ is needed
// to add a new language with built-in types, lexical block scopes, and
// typed literals.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_visitor.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"

#include <gtest/gtest.h>

using namespace dss;
using namespace dss::sem_test;

// `int x;` inside a function body parses through varDecl → varDeclHead,
// which the c-subset `semantics` block declares as a Variable decl
// (name=1, type=0). Should mint one symbol typed I32. The top-level
// `int main()` ALSO mints a symbol — via the c-subset `topLevelDecl`
// declaration with its `kindByChild` discriminator (whenRule =
// funcDefTail → kind=Function). So we expect two symbols total: `main`
// (Function) and `x` (Variable); we find `x` by name.
TEST(SemanticAnalyzerCSubset, FunctionLocalIntDeclTypedAsI32) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { int x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    ASSERT_EQ(model.symbols().size() - 1, 2u) << "main (function) + x (variable)";
    SymbolRecord const* xRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "x") xRec = &model.symbols()[i];
    }
    ASSERT_NE(xRec, nullptr);
    ASSERT_TRUE(xRec->type.valid()) << "the int builtin must resolve to a TypeId";
    EXPECT_EQ(model.lattice().interner().kind(xRec->type), TypeKind::I32);
}

// `int x;` in two DIFFERENT blocks is NOT a redecl — c-subset's
// `block` is declared as a scope opener in the language semantics, so
// each nested block produces its own ScopeId and same-name decls are
// independent symbols.
TEST(SemanticAnalyzerCSubset, NestedBlocksShadowWithoutRedecl) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() {\n"
        "    int x;\n"
        "    { int x; }\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "different blocks → different scopes → no shadow redecl";
    // main (function) + two distinct `x` symbols (one per block scope).
    EXPECT_EQ(model.symbols().size() - 1, 3u);
}

// Use-before-decl inside the same scope resolves through Pass 1's
// pre-minting (G-209 forward refs). Also asserts the use of `x` binds to
// the EXACT declared symbol AND inherits its I32 type — not just "no
// undeclared diagnostic".
TEST(SemanticAnalyzerCSubset, ForwardReferenceWithinBlock) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { x; int x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);

    // main (function) + x (variable). Find x by name.
    ASSERT_EQ(model.symbols().size() - 1, 2u);
    SymbolId xSym{};
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "x") xSym = SymbolId{static_cast<std::uint32_t>(i)};
    }
    ASSERT_TRUE(xSym.valid());
    EXPECT_EQ(model.symbols()[xSym.v].name, "x");

    // Find the `x` USE leaf (the `x;` statement, which precedes the decl).
    // The decl's own name leaf also carries xSym; we want a leaf whose
    // node differs from the decl name node. Both should bind to xSym.
    Tree const& tree = cu->trees()[0];
    NodeId declName = model.symbols()[xSym.v].declNode;
    int boundUses = 0;
    walkPreOrder(tree, [&](TreeCursor const& cursor) {
        NodeId const n = cursor.current();
        if (tree.kind(n) != NodeKind::Token || tree.text(n) != "x") return;
        if (n.v == declName.v) return;  // skip the decl's own name leaf
        EXPECT_EQ(model.symbolAt(n).v, xSym.v) << "use of x binds to x's decl";
        EXPECT_EQ(model.lattice().interner().kind(model.typeAt(n)), TypeKind::I32)
            << "use inherits the declared I32 type";
        ++boundUses;
    });
    // Source has EXACTLY one `x;` use site preceding the decl — strict
    // equality so a future regression that adds (or drops) a use is loud.
    EXPECT_EQ(boundUses, 1) << "the `x;` use site must be present and bound";
}

// IntLiteral and FloatLiteral leaves get the configured TypeId.
TEST(SemanticAnalyzerCSubset, LiteralsAreTyped) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { 42; 3.14; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);

    bool sawI32Lit = false;
    bool sawF64Lit = false;
    model.nodeToType().forEach([&](TreeId, NodeId, TypeId tid) {
        if (!tid.valid()) return;
        auto k = model.lattice().interner().kind(tid);
        if (k == TypeKind::I32) sawI32Lit = true;
        if (k == TypeKind::F64) sawF64Lit = true;
    });
    EXPECT_TRUE(sawI32Lit) << "IntLiteral must be typed I32 per the language semantics";
    EXPECT_TRUE(sawF64Lit) << "FloatLiteral must be typed F64 per the language semantics";
}

// Same-block redeclaration of `int x; int x;` IS a redecl error.
TEST(SemanticAnalyzerCSubset, SameBlockRedeclEmitsError) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { int x; int x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 1u);
}

// SE6 closure: a top-level declaration mints a symbol — variables AND
// functions. `int g = 0; int f() {...}` → two top-level symbols. With
// the `kindByChild` discriminator on `topLevelDecl`, `f` is a
// Function-kind symbol (whenRule = funcDefTail) and `g` is a
// Variable-kind symbol (the discriminator misses, so the static `kind`
// applies).
TEST(SemanticAnalyzerCSubset, TopLevelGlobalsAndFunctionsMintSymbols) {
    auto cu = buildShippedUnit("c-subset", {
        "int g = 0;\n"
        "int f() { return g; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    SymbolRecord const* gRec = nullptr;
    SymbolRecord const* fRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "g") gRec = &model.symbols()[i];
        if (model.symbols()[i].name == "f") fRec = &model.symbols()[i];
    }
    ASSERT_NE(gRec, nullptr) << "top-level global must mint a symbol";
    ASSERT_NE(fRec, nullptr) << "top-level function name must mint a symbol";
    EXPECT_EQ(gRec->kind, DeclarationKind::Variable);
    EXPECT_EQ(fRec->kind, DeclarationKind::Function);
    // `g` is referenced from inside f's body and resolves (no undeclared).
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
}

// SE4: reassigning a `const int x` → exactly one S_ConstViolation.
TEST(SemanticAnalyzerCSubset, ConstReassignmentEmitsConstViolation) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { const int x = 1; x = 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_ConstViolation) EXPECT_EQ(d.actual, "x");
    }
}

// SE4: reassigning a NON-const variable → zero S_ConstViolation.
TEST(SemanticAnalyzerCSubset, NonConstReassignmentIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { int x = 1; x = 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
}

// SE5: a `typedef int Foo;` mints a Type-kind alias symbol carrying the
// aliased TypeId (I32). (c-subset's grammar parses the typedef DECL; the
// alias-in-type-position USE site is exercised generically — see the
// Synth2 typedef tests — because c-subset's `typeBase` is keyword-only.)
TEST(SemanticAnalyzerCSubset, TypedefMintsTypeAliasSymbol) {
    auto cu = buildShippedUnit("c-subset", {
        "typedef int Foo;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    SymbolRecord const* fooRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "Foo") fooRec = &model.symbols()[i];
    }
    ASSERT_NE(fooRec, nullptr);
    EXPECT_EQ(fooRec->kind, DeclarationKind::Type);
    ASSERT_TRUE(fooRec->type.valid());
    EXPECT_EQ(model.lattice().interner().kind(fooRec->type), TypeKind::I32);
}

// SE6 (c-subset): a top-level function with parameters mints a
// Function-kind symbol whose type is a FnSig with the configured param
// and return types. Driven by `topLevelDecl`'s `kindByChild` (whenRule =
// funcDefTail) + the `paramsPath: [0]` / `bodyPath: [1]` resolution into
// the funcDefTail subtree.
TEST(SemanticAnalyzerCSubset, TopLevelFunctionBuildsFnSig) {
    auto cu = buildShippedUnit("c-subset", {
        "int add(int a, int b) { return a; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& interner = model.lattice().interner();
    SymbolRecord const* addRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "add") addRec = &model.symbols()[i];
    }
    ASSERT_NE(addRec, nullptr);
    EXPECT_EQ(addRec->kind, DeclarationKind::Function);
    ASSERT_TRUE(addRec->type.valid());
    ASSERT_EQ(interner.kind(addRec->type), TypeKind::FnSig);
    auto params = interner.fnParams(addRec->type);
    ASSERT_EQ(params.size(), 2u);
    EXPECT_EQ(interner.kind(params[0]), TypeKind::I32);
    EXPECT_EQ(interner.kind(params[1]), TypeKind::I32);
    EXPECT_EQ(interner.kind(interner.fnResult(addRec->type)), TypeKind::I32);
}

// SE6 (c-subset): a correctly-arity call to a user function → no
// diagnostic. The c-subset call rule lives on `postfixExpr` with a
// `ParenOpen` operator-token gate (so `i++` doesn't get treated as a call).
TEST(SemanticAnalyzerCSubset, CorrectArityCallIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(int a) { return a; }\n"
        "int main() { f(1); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotCallable), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ArgCountMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
}

// SE6 (c-subset): too many args to a 1-arg function → exactly one
// S_ArgCountMismatch.
TEST(SemanticAnalyzerCSubset, ExtraArgsEmitArgCountMismatch) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(int a) { return a; }\n"
        "int main() { f(1, 2, 3); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ArgCountMismatch), 1u);
}

// SE6 (c-subset): calling a non-function (a Variable-kind global) →
// exactly one S_NotCallable.
TEST(SemanticAnalyzerCSubset, CallingVariableEmitsNotCallable) {
    auto cu = buildShippedUnit("c-subset", {
        "int x;\n"
        "int main() { x(1); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotCallable), 1u);
}

// SE6 (c-subset): an UNDECLARED CALLEE in a postfix call must emit
// S_UndeclaredIdentifier EXACTLY ONCE — not twice. The call site `ggg(1)`
// structurally overlaps two semantics facets:
//   1. Pass 2 visits the `operand` (which IS the `references` rule) and
//      fails to resolve `ggg` → emits #1.
//   2. Pass 2 then visits the enclosing `postfixExpr` (which IS the
//      `callRules` rule); its callee child IS that very same `operand`,
//      so `checkCall` extracts the same `ggg` identifier and would
//      otherwise emit #2.
// The reporter has a sliding-window dedup that would mask #2, but the
// architecture must not rely on a noise filter to hide a real
// structural double-fire. `checkCall` therefore suppresses its own
// emit when a `references` rule covers the callee subtree (so the
// ref-rule path owns the diagnostic). This test pins THAT decision
// directly — bypassing the dedup by counting raw diagnostics — so a
// future regression that drops the suppression is caught here. The
// tsql peer test (`UnknownFunctionCallEmitsUndeclared`) still passes
// because tsql's callee is a bare `Identifier` token NOT covered by
// any `references` rule, so checkCall remains the sole emitter there.
TEST(SemanticAnalyzerCSubset, UnknownCalleeEmitsExactlyOneUndeclared) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { ggg(1); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    std::size_t gggCount = 0;
    ParseDiagnostic const* gggDiag = nullptr;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_UndeclaredIdentifier
            && d.actual == "ggg") {
            ++gggCount;
            gggDiag = &d;
        }
    }
    EXPECT_EQ(gggCount, 1u)
        << "the undeclared callee must produce EXACTLY ONE "
           "S_UndeclaredIdentifier — not two";
    // The diagnostic must land on the `ggg` token's span. Layout:
    //   "int main() { ggg(1); }"
    //    0123456789012345678901
    // `ggg` is at columns 13..16 (3 chars; half-open span [13, 16)).
    ASSERT_NE(gggDiag, nullptr);
    EXPECT_EQ(gggDiag->span.start(), 13u);
    EXPECT_EQ(gggDiag->span.end(),   16u);
}

// SE6 (c-subset): `i++` is a postfix expression but NOT a call — the
// `operatorToken: ParenOpen` gate on the callRule ensures the engine
// doesn't try to call `i`. Zero S_NotCallable, zero S_ArgCountMismatch.
TEST(SemanticAnalyzerCSubset, PostfixIncrementIsNotACall) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { int i = 0; i++; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotCallable), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ArgCountMismatch), 0u);
}
