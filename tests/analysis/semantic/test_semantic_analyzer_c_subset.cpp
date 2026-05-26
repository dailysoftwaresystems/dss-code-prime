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

#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>

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

// ── GAP A: return-type checking (returnRules facet) ────────────────────────

// `int f() { return 1; }` — I32 result, I32 literal returned → clean.
TEST(SemanticAnalyzerCSubset, ReturnMatchingTypeIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int f() { return 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 0u);
}

// `int f() { return; }` — a non-Void function with a bare `return;` →
// exactly one S_ReturnTypeMismatch (a value is required).
TEST(SemanticAnalyzerCSubset, BareReturnInNonVoidEmitsMismatch) {
    auto cu = buildShippedUnit("c-subset", {
        "int f() { return; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 1u);
}

// `void g() { return 1; }` — a Void function returning a value → exactly
// one S_ReturnTypeMismatch. FIX 5: also pin the diagnostic's byte span — a
// value-return-in-void mismatch is emitted on the returned-VALUE node.
TEST(SemanticAnalyzerCSubset, ValueReturnInVoidEmitsMismatch) {
    auto cu = buildShippedUnit("c-subset", {
        "void g() { return 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 1u);
    // Layout: "void g() { return 1; }"
    //          0123456789012345678901
    // The returned value `1` is at column 18 — half-open span [18, 19). The
    // mismatch lands on the value node (not the whole return statement).
    ParseDiagnostic const* d = nullptr;
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_ReturnTypeMismatch) d = &diag;
    }
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->span.start(), 18u);
    EXPECT_EQ(d->span.end(),   19u);
}

// `void g() { return; }` — a Void function with a bare return → clean.
TEST(SemanticAnalyzerCSubset, BareReturnInVoidIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "void g() { return; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 0u);
}

// A type-mismatched return: `char c` declared, returned from an `int`
// function is fine (Char≠I32 but the returned value is a use whose type is
// Char — Char does not assign into I32 → mismatch). We use a `char`
// parameter returned from an `int` function to force a non-widening
// mismatch. The lattice REJECTS Char→I32 (Char is not in any widening rank;
// see type_rules.hpp isAssignable), so this mismatch is non-vacuous.
// Exactly one S_ReturnTypeMismatch.
TEST(SemanticAnalyzerCSubset, ReturnTypeMismatchOnNonAssignable) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(char c) { return c; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 1u);
}

// FIX 6 companion (non-false-positive guard): returning a matching-typed
// param — `int x` returned from an `int` function — is CLEAN. Proves the
// mismatch check above is not a false positive from over-strict
// assignability (a param-use that DOES assign into the result type passes).
TEST(SemanticAnalyzerCSubset, ReturnAssignableParamIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(int x) { return x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 0u);
}

// FIX 2 (call-result typing): a `return f();` where f returns I32 into an
// I32 function is CLEAN. Pre-fix, the call expression was never result-typed
// in pass2 while the callee identifier WAS typed with f's full FnSig, so the
// return-subtree walk surfaced the FnSig and `isAssignable(I32, FnSig)` =
// false → a spurious S_ReturnTypeMismatch. checkCall now sets the call
// node's type to the callee's result, so the walk sees I32. Zero mismatch.
TEST(SemanticAnalyzerCSubset, ReturnOfCallResultIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int f() { return 1; }\n"
        "int g() { return f(); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 0u)
        << "a call's result type — not its FnSig — must flow to the return check";
    // And the call resolved cleanly besides (no call-shape diagnostics).
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotCallable), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
}

// FIX 2 (genuine mismatch through a call): a `void` function f returning a
// value-call into an `int` function would need f to be non-void; instead we
// exercise a real mismatch THROUGH a call — `char h()` returned from an
// `int` function. h's result is Char, which does NOT assign into I32, so the
// call-result-typed expression yields exactly one S_ReturnTypeMismatch.
TEST(SemanticAnalyzerCSubset, ReturnOfMismatchedCallResultEmitsMismatch) {
    auto cu = buildShippedUnit("c-subset", {
        "char h() { return 'a'; }\n"
        "int g() { return h(); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 1u)
        << "a Char-result call returned into an int function must mismatch";
}

// FIX 3 (crit-8, nested return): a value `return 1;` nested inside an `if`
// inside a `void` function body still checks against the function's result
// type — exactly one S_ReturnTypeMismatch. Proves checkReturn's scope-
// parent-chain walk reaches the enclosing function across the intermediate
// `if`-block scope.
TEST(SemanticAnalyzerCSubset, NestedReturnChecksAgainstEnclosingFunction) {
    auto cu = buildShippedUnit("c-subset", {
        "void g() { if (1) { return 1; } }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 1u)
        << "a nested value-return in a void function must reach the enclosing result";
}

// ── GAP B: duplicate parameter names ───────────────────────────────────────

// `int f(int x, int x) {}` — two params named `x` bind into the same
// (funcDefTail) scope, so the second collides → exactly one
// S_RedeclaredSymbol with a RelatedLocation to the first param.
TEST(SemanticAnalyzerCSubset, DuplicateParamNamesEmitRedecl) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(int x, int x) { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 1u);
    // FIX 8: the related-location must be present AND point at the FIRST `x`
    // param (deterministic). Layout: "int f(int x, int x) { return 0; }"
    //                                 0123456789012345678901
    // The first `x` is at column 10 — half-open span [10, 11).
    ParseDiagnostic const* d = nullptr;
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_RedeclaredSymbol) {
            EXPECT_EQ(diag.actual, "x");
            d = &diag;
        }
    }
    ASSERT_NE(d, nullptr);
    ASSERT_EQ(d->related.size(), 1u) << "the duplicate param must point back at the first";
    EXPECT_EQ(d->related[0].span.start(), 10u);
    EXPECT_EQ(d->related[0].span.end(),   11u);
}

// ── GAP C: break/continue outside loop (loopControls facet) ────────────────

// `while (1) { break; }` — a break inside a loop body → clean.
TEST(SemanticAnalyzerCSubset, BreakInsideLoopIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { while (1) { break; } return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ControlOutsideLoop), 0u);
}

// A bare `break;` in a function body but outside any loop → exactly one
// S_ControlOutsideLoop. FIX 5: also pin the diagnostic's exact byte span.
TEST(SemanticAnalyzerCSubset, BreakOutsideLoopEmitsDiagnostic) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { break; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ControlOutsideLoop), 1u);
    // Layout: "int main() { break; return 0; }"
    //          0123456789012345678901234567890
    // `break` starts at column 13; the breakStmt node spans `break;` — a
    // half-open span [13, 19). Pin both ends so a regression that drifts the
    // emit point off the breakStmt node is loud.
    ParseDiagnostic const* d = nullptr;
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_ControlOutsideLoop) d = &diag;
    }
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->span.start(), 13u);
    EXPECT_EQ(d->span.end(),   19u);
}

// FIX 4 (switch is a break-context): `break` inside a `switch` body → clean.
// c-subset's `switchStmt` is a configured loopRules (break-context) entry,
// so a break there is NOT outside-loop.
TEST(SemanticAnalyzerCSubset, BreakInsideSwitchIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(){ switch(1){ case 1: break; } return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ControlOutsideLoop), 0u)
        << "switch is a configured break-context";
}

// FIX 4 (nested-loop depth): a break in an inner loop nested in an outer
// loop is clean (depth ≥ 1 throughout the inner body).
TEST(SemanticAnalyzerCSubset, NestedLoopBreakIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(){ while(1){ while(1){ break; } } return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ControlOutsideLoop), 0u);
}

// FIX 4 (depth decrement after a loop closes): a break placed AFTER a loop
// body has closed is back at depth 0 → exactly one S_ControlOutsideLoop.
// Proves the loop-depth increment is scoped to the loop subtree and is
// correctly NOT carried past the loop's closing brace.
TEST(SemanticAnalyzerCSubset, BreakAfterLoopIsOutsideLoop) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(){ while(1){ } break; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ControlOutsideLoop), 1u)
        << "a break after the loop closes is back at depth 0";
}

// ── GAP F: split `# include` ───────────────────────────────────────────────

// `# include "x.h"` (whitespace between `#` and `include`) resolves the
// include directive — `#` and `include` now tokenize separately, and the
// EmptySpace between them is skipped by the cursor. We add the directive as
// in-memory with an include dir holding the target.
TEST(SemanticAnalyzerCSubset, SpacedIncludeIsRecognized) {
    // Build a CU with a header on the include path and a spaced `# include`.
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_gapF_include_test";
    fs::create_directories(dir);
    {
        std::ofstream(dir / "x.h", std::ios::binary)
            << "int helper() { return 1; }\n";
    }
    auto schema = loadShippedSchema("c-subset");
    UnitBuilder builder{schema};
    builder.addIncludeDir(dir);
    builder.addInMemory("# include \"x.h\"\nint main() { return 0; }\n", "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    // The directive resolved → a cross-ref edge + the header tree was loaded.
    EXPECT_EQ(cu->trees().size(), 2u);
    EXPECT_EQ(cu->crossRefs().size(), 1u);
    EXPECT_FALSE(hasCode(cu->driverDiagnostics(), DiagnosticCode::D_UnresolvedImport));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
