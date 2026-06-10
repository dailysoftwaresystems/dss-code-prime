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
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
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

// SE-arrays (HR9): a `[N]` declarator suffix folds the element type into
// Array<elem, N>. `int a[10];` mints a symbol typed Array<I32, 10> — the
// constant length comes from a semantic-time literal eval, config-driven via
// the `varDeclHead` declaration's `arraySuffix` descriptor.
TEST(SemanticAnalyzerCSubset, ArrayDeclaratorTypedAsArray) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { int a[10]; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* aRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "a") aRec = &model.symbols()[i];
    }
    ASSERT_NE(aRec, nullptr);
    ASSERT_TRUE(aRec->type.valid());
    ASSERT_EQ(ti.kind(aRec->type), TypeKind::Array);
    ASSERT_EQ(ti.scalars(aRec->type).size(), 1u);
    EXPECT_EQ(ti.scalars(aRec->type)[0], 10);
    ASSERT_EQ(ti.operands(aRec->type).size(), 1u);
    EXPECT_EQ(ti.kind(ti.operands(aRec->type)[0]), TypeKind::I32);
}

// SE-arrays: a non-constant length (`int a[n]`) must fail loud rather than
// guess. The engine emits S_NonConstantArrayLength and leaves the type
// unresolved (no silent pointer decay, no assumed length).
TEST(SemanticAnalyzerCSubset, NonConstantArrayLengthEmitsDiagnostic) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(int n) { int a[n]; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 1u);
}

// SE-arrays: an empty-bracket declarator (`int a[]`) has no length — a DIFFERENT
// path from `[n]` (the length node lands on the `]` token, not an identifier).
// Must also fail loud.
TEST(SemanticAnalyzerCSubset, EmptyArrayLengthEmitsDiagnostic) {
    auto cu = buildShippedUnit("c-subset", { "int main() { int a[]; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 1u);
}

// SE-arrays: a non-decimal length exercises the shared decodeInteger through the
// NEW semantic consumer — `0x10` must decode to 16 (radix handling), not be
// rejected as non-constant.
TEST(SemanticAnalyzerCSubset, HexArrayLengthDecodes) {
    auto cu = buildShippedUnit("c-subset", { "int main() { int a[0x10]; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* aRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "a") aRec = &model.symbols()[i];
    ASSERT_NE(aRec, nullptr);
    ASSERT_EQ(ti.kind(aRec->type), TypeKind::Array);
    EXPECT_EQ(ti.scalars(aRec->type)[0], 16);
}

// SE-arrays: a constant length that decodes but exceeds the signed length the
// lattice stores must NOT wrap to a negative length — fail loud with the
// dedicated S_ArrayLengthOutOfRange (regression for a silent sign-flip).
TEST(SemanticAnalyzerCSubset, OutOfRangeArrayLengthEmitsDiagnostic) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { int a[0xFFFFFFFFFFFFFFFF]; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayLengthOutOfRange), 1u);
}

// SE-pointers (G5): `int *p` declarator → Ptr<I32>; `int **pp` → Ptr<Ptr<I32>>.
// The declarator stars wrap the base type one level each (declarator-depth).
TEST(SemanticAnalyzerCSubset, PointerDeclaratorTypedAsPtr) {
    auto cu = buildShippedUnit("c-subset", {
        "void f() { int *p; int **pp; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* p = nullptr;
    SymbolRecord const* pp = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "p")  p  = &model.symbols()[i];
        if (model.symbols()[i].name == "pp") pp = &model.symbols()[i];
    }
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(ti.kind(p->type), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(ti.operands(p->type)[0]), TypeKind::I32);
    ASSERT_NE(pp, nullptr);
    ASSERT_EQ(ti.kind(pp->type), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(ti.operands(pp->type)[0]), TypeKind::Ptr);          // Ptr<Ptr<I32>>
    EXPECT_EQ(ti.kind(ti.operands(ti.operands(pp->type)[0])[0]), TypeKind::I32);
}

// D-LANG-POINTER-VOID-CONVERT (step 13.2, 2026-06-02): `void *p` types
// as `Ptr<Void>` — the existing pointer-declarator machinery handles
// the Void element type without special-casing (the grammar parses
// `void` + StarOp; resolveTypeNode wraps the Void TypeId in
// interner.pointer()).
TEST(SemanticAnalyzerCSubset, VoidStarDeclaratorTypedAsPtrVoid) {
    auto cu = buildShippedUnit("c-subset", { "void f() { void *p; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* p = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "p") p = &model.symbols()[i];
    }
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(ti.kind(p->type), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(ti.operands(p->type)[0]), TypeKind::Void)
        << "void* must intern as Ptr<Void> (the untyped-memory case "
           "of Void's dual semantics — distinct from void-return)";
}

// D-LANG-POINTER-VOID-CONVERT: an extern function taking `void*`
// argument types correctly + the call site passing a `char*` arg
// must accept without diagnostic (C-standard §6.3.2.3 — c-subset
// declares both directions implicit in pointerConversions).
TEST(SemanticAnalyzerCSubset, CharStarToVoidStarArgImplicit) {
    auto cu = buildShippedUnit("c-subset", {
        "extern int handler(void* p);\n"
        "int main() {\n"
        "    char* s;\n"
        "    return handler(s);\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // Strict countCode pin (replaces an earlier any-bool sawMismatch
    // loop that would have silently passed a wrong-code regression
    // — e.g., an S_ReturnTypeMismatch firing in place of S_TypeMismatch
    // would have masked the assertion).
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "char* → void* must be implicit in c-subset "
           "(implicitToVoidPtr: true)";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArgCountMismatch), 0u);
}

// D-LANG-POINTER-VOID-CONVERT: the reverse direction (`void*` →
// `char*`) is also implicit under C semantics (c-subset declares
// `implicitFromVoidPtr: true`) — C++ would forbid this without
// an explicit cast.
TEST(SemanticAnalyzerCSubset, VoidStarToCharStarArgImplicit) {
    auto cu = buildShippedUnit("c-subset", {
        "extern int handler(const char* s);\n"
        "extern void* alloc(int n);\n"
        "int main() {\n"
        "    return handler(alloc(16));\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "void* → const char* must be implicit in c-subset "
           "(implicitFromVoidPtr: true). When C++ frontend lands, "
           "it would declare implicitFromVoidPtr: false and this "
           "direction would require an explicit cast.";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArgCountMismatch), 0u);
}

// D-LANG-POINTER-VOID-CONVERT negative pin: distinct typed pointers
// remain mismatch under c-subset (only `void*` ↔ `T*` is implicit;
// `int*` → `char*` requires an explicit cast even in C).
TEST(SemanticAnalyzerCSubset, DistinctTypedPointersRemainMismatch) {
    auto cu = buildShippedUnit("c-subset", {
        "extern int handler(int* p);\n"
        "int main() {\n"
        "    char* s;\n"
        "    return handler(s);\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // Pin EXACTLY ONE S_TypeMismatch (not duplicate cascade) AND
    // zero adjacent mismatch codes — replaces the loose any-bool
    // sawMismatch loop that would have admitted unrelated mismatch
    // codes (S_ReturnTypeMismatch / S_ArgCountMismatch) as satisfying
    // the assertion.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "char* → int* must NOT be implicit even in c-subset — "
           "void* is the only universal-pointer special case; "
           "ordinary typed pointers require an explicit cast";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArgCountMismatch), 0u);
}

// D-LANG-POINTER-VOID-CONVERT (step 13.2 audit fold): the
// `pointerConversions`-gated `isAssignable` now reaches THREE
// check sites in semantic_analyzer.cpp — `checkCall`'s call-arg
// loop, `checkReturn`'s return-type check, and pass-2's
// declaration-init arm. The original 13.2 tests exercised only
// the call-arg site; these tests add return-direction + init-
// direction pins (and a negative-pin via the return path).
TEST(SemanticAnalyzerCSubset, VoidStarReturnFromTypedPtrImplicit) {
    auto cu = buildShippedUnit("c-subset", {
        "void* f(int* p) { return p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u)
        << "int* → void* via return must be implicit in c-subset "
           "(implicitToVoidPtr: true)";
}

TEST(SemanticAnalyzerCSubset, TypedPtrReturnFromVoidStarImplicit) {
    auto cu = buildShippedUnit("c-subset", {
        "int* f(void* p) { return p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u)
        << "void* → int* via return must be implicit in c-subset "
           "(implicitFromVoidPtr: true). C++ would forbid.";
}

TEST(SemanticAnalyzerCSubset, DistinctTypedReturnRemainsMismatch) {
    auto cu = buildShippedUnit("c-subset", {
        "int* f(char* p) { return p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 1u)
        << "char* → int* via return must NOT be implicit even in "
           "c-subset (only void* gets the universal-pointer pass).";
}

// D-LANG-NULL-POINTER-CONSTANT (step 13.3, 2026-06-02): per C §6.3.2.3.3,
// the integer literal `0` is a null pointer constant — convertible to
// ANY pointer type without a cast. c-subset declares
// `nullPointerConstantFromIntegerZero: true` in its `pointerConversions`
// block. These tests pin all three `isAssignable` call sites
// (call-arg, return, init) AND a strict-reject case for non-zero
// integer literals.
TEST(SemanticAnalyzerCSubset, NullPointerConstantAdmitsAsVoidStarArg) {
    auto cu = buildShippedUnit("c-subset", {
        "extern void f(void* p);\n"
        "int main() { f(0); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // F5 audit fix (6-agent 2nd-order, step 13.3a): pair the
    // countCode(target) pin with `!hasErrors()` so a future
    // wrong-code regression (e.g. a new S_NullPointerInvalid) can't
    // silently satisfy the 0-count assertion.
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u);
}

TEST(SemanticAnalyzerCSubset, NullPointerConstantAdmitsAsTypedPointerArg) {
    auto cu = buildShippedUnit("c-subset", {
        "extern void f(int* p);\n"
        "int main() { f(0); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // C §6.3.2.3.3: NULL pointer constant converts to ANY pointer type
    // (not just void*) without a cast. F5 audit fix: pair countCode
    // with !hasErrors so wrong-code regressions can't silently pass.
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "literal 0 must convert to int* without an explicit cast";
}

TEST(SemanticAnalyzerCSubset, NonZeroIntegerLiteralRejectsAsPointerArg) {
    auto cu = buildShippedUnit("c-subset", {
        "extern void f(void* p);\n"
        "int main() { f(1); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // Negative pin: ONLY the literal `0` admits as null pointer
    // constant — `1` (or any non-zero int) must NOT silently convert.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "non-zero int literal must NOT be admitted as a null "
           "pointer constant — only value-0 qualifies per C "
           "§6.3.2.3.3";
}

TEST(SemanticAnalyzerCSubset, NullPointerConstantAdmitsAsReturn) {
    auto cu = buildShippedUnit("c-subset", {
        "int* f() { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors());  // F5 audit fix
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u)
        << "`return 0;` from an int*-returning function is a null "
           "pointer conversion per C §6.3.2.3.3";
}

TEST(SemanticAnalyzerCSubset, NullPointerConstantAdmitsAsInit) {
    auto cu = buildShippedUnit("c-subset", {
        "void f() { int* p = 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors());  // F5 audit fix
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "`int* p = 0;` initializer is a null pointer constant";
}

// 2nd-order audit pin (code-reviewer Critical, step 13.3a): the
// initial F1 fix used arity-based `OperatorTable.lookup(tk, Prefix)`,
// which would have incorrectly matched binary-arithmetic operator
// tokens (MinusOp/PlusOp/StarOp/BitAndOp are registered for BOTH
// Prefix and Infix arities at the SAME SchemaTokenId in
// c-subset.lang.json:128). The position-based fix only fires when
// the FIRST visible child is a prefix-capable token — distinguishing
// `-x` (first-position) from `a-b` (first-position is `a`). This
// pin asserts S_TypeMismatch STILL fires on `f(1+1)` where `f`
// takes a pointer; pre-position-fix it would have silently
// cascade-suppressed.
TEST(SemanticAnalyzerCSubset, InfixArithmeticStillFiresMismatchAtCallArg) {
    auto cu = buildShippedUnit("c-subset", {
        "extern void f(char* p);\n"
        "int main() { f(1+1); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "int (from `1+1`) → char* must fire mismatch — the "
           "subtreeType operator-stop must NOT match PlusOp on its "
           "INFIX usage at this wrapper (first-position child is "
           "the integer literal, not the operator)";
}

// 2nd-order audit pin (silent-failure B-1, step 13.3a): document
// the CURRENT cascade-suppression behavior on `f(-0)` so a future
// tightening of subtreeType (pass-2 expression typing closure)
// EXPLICITLY surfaces this case. Today the operator-stop returns
// InvalidType for the `-0` wrapper (MinusOp at first position is
// a registered Prefix operator); the call-arg check then
// silently suppresses via `if (!argTy.valid()) continue;`. NO
// diagnostic fires. This is the documented trade-off: false-
// negative on Prefix-`-` rather than false-positive on `a-b`. The
// fix is pass-2 expression typing of unary wrappers (anchored at
// D-SEMANTIC-SUBTREETYPE-TRANSPARENT-WRAPPERS full closure). If a
// future tightening EITHER emits a diagnostic for `f(-0)` OR
// admits `-0` as a null-pointer constant, this test will fail and
// the maintainer will need to consciously update the pin.
TEST(SemanticAnalyzerCSubset, NegativeZeroSilentlySuppressedAtCallArg) {
    auto cu = buildShippedUnit("c-subset", {
        "extern void f(void* p);\n"
        "int main() { f(-0); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // CURRENT documented behavior: zero diagnostics. Pre-13.3a
    // (with the original looser arity-check) and post-13.3a (with
    // the position-based stop) BOTH produce zero diagnostics here
    // — silent-suppression via cascade. Pin so future closures
    // surface the change.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u);
}

// F1 audit-fix pin (6-agent 2nd-order, step 13.3a): paren-wrapped
// distinct-typed pointers must STILL fire S_TypeMismatch. Pre-fix,
// subtreeType's operator-stop heuristic matched ParenOpen (shared
// SchemaTokenId with the postfix call operator), which would have
// suppressed the diagnostic. The narrowed operator-stop (Prefix +
// Ternary only) excludes ParenOpen.
TEST(SemanticAnalyzerCSubset, ParenWrappedDistinctTypedPointersStillMismatch) {
    auto cu = buildShippedUnit("c-subset", {
        "extern int handler(int* p);\n"
        "int main() {\n"
        "    char* s;\n"
        "    return handler((s));\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "paren-wrapped char* → int* must still fire mismatch — "
           "operator-stop must NOT match ParenOpen (which shares "
           "the postfix-call SchemaTokenId with paren-wrapping)";
}

// D-LANG-POINTER-VOID-CONVERT audit fold (silent-failure 2nd-order H2):
// the `subtreeType()` swap in `checkMemberAccess` (semantic_analyzer.cpp
// lhsType lookup) had zero existing test coverage — pre-fix the
// `typeAt(lhsNode)` returning InvalidType for bare-identifier wrappers
// silently bypassed S_NotAPointer / S_NotAComposite / field-type
// write-back. These 3 tests pin both the positive arrow-access path
// AND the negative non-pointer-deref reject — without them, a future
// regression in the swap (e.g. reverting to typeAt) would silently
// pass.
TEST(SemanticAnalyzerCSubset, StructMemberAccessViaArrowOnBareRefIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int x; };\n"
        "void f(struct S *p) { p->x = 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NotAPointer), 0u)
        << "p->x where p is Ptr<Struct> must NOT fire S_NotAPointer";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NotAComposite), 0u)
        << "p->x where Struct has field x must NOT fire S_NotAComposite";
}

TEST(SemanticAnalyzerCSubset, ArrowAccessOnNonPointerFiresLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int x; };\n"
        "void f(int n) { n->x = 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NotAPointer), 1u)
        << "n->x where n is int must fire EXACTLY ONE S_NotAPointer "
           "— pre-subtreeType swap the bare-ref wrapper's InvalidType "
           "silently suppressed this diagnostic class entirely";
}

TEST(SemanticAnalyzerCSubset, StructDotMemberAccessOnBareRefIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int x; };\n"
        "void f() { struct S s; s.x = 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NotAPointer), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NotAComposite), 0u)
        << "Direct `s.x` access on bare Struct ref must be clean — "
           "the swap admits both arrow-arm and dot-arm equally";
}

// SE-pointers (G5): a pointer parameter types as Ptr in the FnSig.
TEST(SemanticAnalyzerCSubset, PointerParamInFnSig) {
    auto cu = buildShippedUnit("c-subset", { "void f(int *p) {}\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* f = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "f") f = &model.symbols()[i];
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(ti.kind(f->type), TypeKind::FnSig);
    auto params = ti.fnParams(f->type);
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(ti.kind(params[0]), TypeKind::Ptr);
}

// SE-arrays: a GLOBAL array (`int g[10];`) — the suffix nests under
// `topLevelDecl → varDeclTail → arrayDeclSuffix`, exercising applyArraySuffix's
// descendant scan. Must type as Array<I32,10> just like the local case.
TEST(SemanticAnalyzerCSubset, GlobalArrayDeclaratorTypedAsArray) {
    auto cu = buildShippedUnit("c-subset", { "int g[10];\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* gRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "g") gRec = &model.symbols()[i];
    ASSERT_NE(gRec, nullptr);
    ASSERT_EQ(ti.kind(gRec->type), TypeKind::Array);
    EXPECT_EQ(ti.scalars(gRec->type)[0], 10);
    EXPECT_EQ(ti.kind(ti.operands(gRec->type)[0]), TypeKind::I32);
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

// D2: compound-assignment const-correctness. The c-subset grammar's
// operator table registers every compound-assign token (`+=`, `-=`, `<<=`,
// …) as an infix operator at the same precedence/associativity as `=`, so
// `x += 2;` parses as a binaryExpr with the compound-assign token as its
// operator. Each compound-assign token has its own `assignments` entry, so
// reassigning a const through ANY of them emits S_ConstViolation exactly
// like a plain `=`.
TEST(SemanticAnalyzerCSubset, CompoundAssignToConstEmitsConstViolation) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { const int x = 1; x += 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
    // `x` is the compound-assign LHS, which counts as a use — so the
    // varDeclHead `warnIfUnused:true` opt-in does NOT spuriously fire.
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_ConstViolation) EXPECT_EQ(d.actual, "x");
    }
}

// D2: `*=` against a const → exactly one S_ConstViolation (a second
// compound operator, proving the entry is per-token, not just `+=`).
TEST(SemanticAnalyzerCSubset, CompoundStarAssignToConstEmitsConstViolation) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { const int x = 4; x *= 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u);
}

// D2: `<<=` against a const → exactly one S_ConstViolation (a third,
// three-char compound operator).
TEST(SemanticAnalyzerCSubset, CompoundShlAssignToConstEmitsConstViolation) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { const int x = 1; x <<= 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u);
}

// D2: a NON-const variable compound-assigned (`y <<= 2;`) → zero
// S_ConstViolation. Proves the compound-assign entries gate on const-ness,
// not on the operator alone.
TEST(SemanticAnalyzerCSubset, CompoundAssignToNonConstIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { int y = 1; y <<= 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u);
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

// ── D8: unused-variable warning (warnIfUnused opt-in) ──────────────────────

// `int main(){ int unused; int used=1; return used; }` — `unused` is never
// referenced → exactly one S_UnusedVariable (a WARNING) whose `actual` is
// "unused" and whose span covers the declaration. `used` IS referenced in
// the return, so it does NOT warn.
TEST(SemanticAnalyzerCSubset, UnusedLocalEmitsWarning) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(){ int unused; int used=1; return used; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 1u);
    ParseDiagnostic const* d = nullptr;
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_UnusedVariable) d = &diag;
    }
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->actual, "unused") << "the warning names the unused variable";
    EXPECT_EQ(d->severity, DiagnosticSeverity::Warning);
    // The span points at `unused`'s declaration (the varDeclHead node).
    // Layout: "int main(){ int unused; int used=1; return used; }"
    //          0123456789012345678901234567890
    // The varDeclHead `int unused` starts at column 12 (`int`) and ends at
    // the end of `unused` (column 22, half-open). Pin both ends so a
    // regression that drifts the emit point off the decl node is loud.
    EXPECT_EQ(d->span.start(), 12u);
    EXPECT_EQ(d->span.end(),   22u);
}

// A function PARAMETER that is unused does NOT warn — c-subset sets
// `warnIfUnused` on `varDeclHead` (locals) but NOT on `param`. This proves
// the per-declaration opt-in: same engine, same empty use-set, but no
// warning because the declaration didn't opt in.
TEST(SemanticAnalyzerCSubset, UnusedParamDoesNotWarn) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(int unusedParam){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u)
        << "unused params are intentional — param decls do not opt in";
}

// A USED local does not warn (companion non-false-positive guard): every
// local is referenced, so zero S_UnusedVariable.
TEST(SemanticAnalyzerCSubset, OneUnusedLocalAmongUsedWarnsOnlyForUnused) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(){ int a=1; int b=2; return a; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // `a` is used; `b` is NOT — so exactly one warning, for `b`. Proves the
    // check fires per-symbol on the actual empty-use-set, not blanket.
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 1u);
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_UnusedVariable) EXPECT_EQ(diag.actual, "b");
    }
}

// An unused TOP-LEVEL global does NOT warn — c-subset does not set
// `warnIfUnused` on `topLevelDecl`. Proves globals are exempt.
TEST(SemanticAnalyzerCSubset, UnusedGlobalDoesNotWarn) {
    auto cu = buildShippedUnit("c-subset", {
        "int unusedGlobal;\n"
        "int main(){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u)
        << "globals do not opt in to the unused-variable warning";
}

// A WRITE-ONLY local (assigned but never read) does NOT warn. The
// unused-variable scope is "never referenced" ONLY: an assignment LHS
// counts as a use, so `x = 5;` consumes `x`'s use-set even though `x`'s
// value is never read. This pins the documented scope boundary — detecting
// assigned-but-never-read is deferred to the optimizer/dataflow phase, not
// the semantic analyzer.
TEST(SemanticAnalyzerCSubset, WriteOnlyLocalDoesNotWarn) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(){ int x; x = 5; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u)
        << "an assignment LHS counts as a use — write-only stays for the optimizer";
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

// ── FF11: angle-include resolves a NEUTRAL JSON DESCRIPTOR + injects its ──────
// ── externs at the SEMANTIC phase + GOAL-2 (user decl wins) ──────────────────
//
// These pin the DESCRIPTOR model (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC) that
// REPLACED cycle-21's source-`.h` load. An angle `#include <io.h>` no longer
// pulls in a parsed c-subset header; it resolves to `io.json` on the system
// dir, and the semantic phase reads that descriptor and MINTS its externs into
// scope BEFORE Pass 2 (the builtinFunctions analogue) — so a call resolves.
// A descriptor symbol a USER declaration already claims is SKIPPED (goal-2:
// user decl wins; no duplicate symbol). The old cycle-21 tree-level
// S_RedeclaredSymbol no longer applies (a descriptor is not a tree).

namespace {
using dss::test_support::Location;
using dss::test_support::ScratchDir;

// Build a c-subset CU whose `main.c` source is `mainSrc`, with a NEUTRAL JSON
// descriptor `descName` (content `descJson`) written into `sysDir` (a SYSTEM
// dir reachable by the angle form `#include <...>`). The descriptor FILE must
// outlive the returned CU — the SEMANTIC phase reads it at `analyze()` time —
// so the caller owns the `ScratchDir` (its dtor cleans up AFTER `analyze`),
// rather than the helper deleting the dir before returning (which would race
// the read).
[[nodiscard]] std::shared_ptr<CompilationUnit const>
buildAngleDescriptorUnit(ScratchDir const& sysDir,
                         std::string const& descName,
                         std::string const& descJson,
                         std::string const& mainSrc) {
    std::ofstream(sysDir.path() / descName, std::ios::binary) << descJson;
    auto schema = loadShippedSchema("c-subset");
    UnitBuilder builder{schema};
    builder.addSystemDir(sysDir.path());
    builder.addInMemory(mainSrc, "main.c");
    return std::make_shared<CompilationUnit>(std::move(builder).finish());
}

// Count how many minted symbols carry `name` (1 for a clean injection; >1 is a
// duplicate-symbol bug). Walks the whole symbol table (slot 0 unused).
[[nodiscard]] std::size_t countSymbolsNamed(SemanticModel const& model,
                                            std::string_view name) {
    std::size_t n = 0;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == name) ++n;
    }
    return n;
}
} // namespace

// The C-faithful angle include: `#include <io.h>` resolves `io.json` (which
// declares `puts`), the semantic phase injects `puts`, and the program's call
// `puts("hi")` RESOLVES — no S_UndeclaredIdentifier. This is the semantic-tier
// end-to-end pin: REMOVE the descriptor injection (semantic_analyzer.cpp) and
// `puts` is undeclared → RED. The injected `puts` carries a FnSig and the
// model records exactly one `shippedExterns` row for it.
TEST(SemanticAnalyzerCSubset, FF11AngleIncludeResolvesPutsViaDescriptor) {
    ScratchDir sysDir{Location::Temp, "ff11-desc"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "io.json",
        R"({ "header": "io.h", "library": { "pe": "msvcrt.dll", "elf": "libc.so.6" },
             "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ] })",
        "#include <io.h>\nint main() { puts(\"hi\"); return 0; }\n");
    // NO second tree — the descriptor is not parsed source.
    ASSERT_EQ(cu->trees().size(), 1u) << "a descriptor is recorded, not loaded as a Tree";
    EXPECT_EQ(cu->shippedLibDescriptors().size(), 1u);
    assertNoBuilderErrors(*cu);

    auto model = analyze(cu);
    // The use of `puts` resolved against the injected descriptor symbol.
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "puts must resolve via the injected descriptor extern";
    EXPECT_FALSE(hasCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol));

    // Exactly one `puts` symbol, and exactly one descriptor-extern row, with a
    // FnSig signature + the descriptor's library.
    EXPECT_EQ(countSymbolsNamed(model, "puts"), 1u);
    ASSERT_EQ(model.shippedExterns().size(), 1u)
        << "one descriptor symbol minted (puts)";
    auto const& ext = model.shippedExterns()[0];
    EXPECT_EQ(ext.name, "puts");
    EXPECT_TRUE(ext.isFunction);
    // Model 3: `library` is a per-object-format map carried target-agnostically
    // through the semantic model (the active target's entry is selected later,
    // at compile_pipeline). Assert both format entries round-trip.
    ASSERT_EQ(ext.library.size(), 2u);
    EXPECT_EQ(ext.library.at("pe"), "msvcrt.dll");
    EXPECT_EQ(ext.library.at("elf"), "libc.so.6");
    ASSERT_TRUE(ext.signature.valid());
    EXPECT_EQ(model.lattice().interner().kind(ext.signature), TypeKind::FnSig);
}

// GOAL-2 BEHAVIOR PIN: a program that BOTH `#include <io.h>` (descriptor
// declares `puts`) AND writes its OWN `extern char puts(int x);` — the USER
// DECLARATION WINS. The descriptor injection SKIPS a name a user decl already
// claimed, so there is exactly ONE `puts` symbol (the user's), and NO
// descriptor-extern row for `puts` (it was skipped). No duplicate symbol, no
// double-bound import. The old tree-level S_RedeclaredSymbol does NOT fire (a
// descriptor is not a tree) — this is the deliberate descriptor-model behavior.
TEST(SemanticAnalyzerCSubset, FF11AngleIncludePlusInlineExternUserDeclWins) {
    ScratchDir sysDir{Location::Temp, "ff11-desc"};
    // The descriptor declares TWO symbols: `puts` (which the user ALSO declares
    // — must be skipped) and `fputs` (which the user does NOT — must be
    // injected). The pair makes the goal-2 skip provably SELECTIVE: RED if it
    // skips nothing (puts doubled) AND RED if it skips everything (fputs lost).
    auto cu = buildAngleDescriptorUnit(
        sysDir, "io.json",
        R"({ "header": "io.h", "library": { "pe": "msvcrt.dll", "elf": "libc.so.6" },
             "symbols": [ { "name": "puts",  "signature": "fn(ptr<char>) -> i32" },
                          { "name": "fputs", "signature": "fn(ptr<char>) -> i32" } ] })",
        "extern char puts(int x);\n"
        "#include <io.h>\n"
        "int main() { return 0; }\n");
    ASSERT_EQ(cu->trees().size(), 1u);
    assertNoBuilderErrors(*cu);

    auto model = analyze(cu);
    // The user's decl is the SOLE `puts` — the descriptor symbol was skipped.
    EXPECT_EQ(countSymbolsNamed(model, "puts"), 1u)
        << "user decl wins — the descriptor's puts is skipped (no duplicate)";
    // The UNCLAIMED descriptor symbol `fputs` IS injected (selective skip).
    EXPECT_EQ(countSymbolsNamed(model, "fputs"), 1u)
        << "an unclaimed descriptor symbol must still inject";
    ASSERT_EQ(model.shippedExterns().size(), 1u)
        << "exactly one descriptor extern row — fputs only (puts skipped)";
    EXPECT_EQ(model.shippedExterns()[0].name, "fputs");
    for (auto const& ext : model.shippedExterns()) {
        EXPECT_NE(ext.name, "puts")
            << "a descriptor symbol a user decl claimed must NOT be injected";
    }
    // The user's `puts` carries the user's signature (`char(int)`), not the
    // descriptor's `i32(ptr<char>)` — concrete proof the user decl is the one
    // that survived. The user's first param is the int (I32) it declared.
    SymbolRecord const* userPuts = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "puts") userPuts = &model.symbols()[i];
    }
    ASSERT_NE(userPuts, nullptr);
    ASSERT_TRUE(userPuts->type.valid());
    auto const& ti = model.lattice().interner();
    ASSERT_EQ(ti.kind(userPuts->type), TypeKind::FnSig);
    auto const params = ti.fnParams(userPuts->type);
    ASSERT_EQ(params.size(), 1u) << "the user's puts(int) has one int param";
    EXPECT_EQ(ti.kind(params[0]), TypeKind::I32)
        << "the surviving puts is the USER's (int param), not the descriptor's "
           "(ptr<char> param)";
}

// DEDUP: the SAME descriptor `#include`d TWICE injects its symbol EXACTLY ONCE.
// The resolver records the descriptor path per directive (twice here), so the
// semantic injection must de-dup on canonical path. RED (two `puts` symbols /
// two extern rows) if the path-dedup is removed.
TEST(SemanticAnalyzerCSubset, FF11SameDescriptorIncludedTwiceInjectsOnce) {
    ScratchDir sysDir{Location::Temp, "ff11-desc"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "io.json",
        R"({ "header": "io.h", "library": { "pe": "msvcrt.dll", "elf": "libc.so.6" },
             "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ] })",
        "#include <io.h>\n"
        "#include <io.h>\n"
        "int main() { puts(\"hi\"); return 0; }\n");
    ASSERT_EQ(cu->trees().size(), 1u);
    // Two directives → two recorded descriptor paths (deduped at injection).
    EXPECT_EQ(cu->shippedLibDescriptors().size(), 2u);
    assertNoBuilderErrors(*cu);

    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
    EXPECT_EQ(countSymbolsNamed(model, "puts"), 1u)
        << "the same descriptor twice injects puts ONCE — not per directive";
    EXPECT_EQ(model.shippedExterns().size(), 1u)
        << "exactly one descriptor-extern row for puts";
}

// DEDUP across MANY symbols + a repeated descriptor: every DISTINCT descriptor
// symbol is injected EXACTLY ONCE, and a descriptor included twice does not
// double-inject. Mirrors the old window-defeating guard's spirit (many symbols,
// a duplicated one) under the descriptor model: a name is minted at most once
// regardless of how many descriptors (or repeats) declare it. Each of the five
// names (dup + s0..s3) is used by the program so all must resolve.
TEST(SemanticAnalyzerCSubset, FF11MultipleDescriptorsEachSymbolInjectedOnce) {
    ScratchDir sysDir{Location::Temp, "ff11-desc-multi"};
    auto writeDesc = [&](std::string const& stem, std::string const& sym) {
        std::ofstream(sysDir.path() / (stem + ".json"), std::ios::binary)
            << R"({ "header": ")" << stem << R"(.h", "library": { "pe": "msvcrt.dll" }, )"
            << R"("symbols": [ { "name": ")"
            << sym << R"(", "signature": "fn() -> i32" } ] })";
    };
    writeDesc("dup", "dup");
    for (int i = 0; i < 4; ++i) writeDesc("s" + std::to_string(i), "s" + std::to_string(i));

    auto schema = loadShippedSchema("c-subset");
    UnitBuilder builder{schema};
    builder.addSystemDir(sysDir.path());
    // <dup.h> is included TWICE (bracketing the others); each of dup,s0..s3 is
    // called so all five must resolve, and dup must be minted only once.
    builder.addInMemory(
        "#include <dup.h>\n"
        "#include <s0.h>\n"
        "#include <s1.h>\n"
        "#include <s2.h>\n"
        "#include <s3.h>\n"
        "#include <dup.h>\n"
        "int main() { return dup() + s0() + s1() + s2() + s3(); }\n",
        "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    ASSERT_EQ(cu->trees().size(), 1u) << "descriptors are not parsed Trees";
    // Six directives → six recorded paths (dup repeated); deduped at injection.
    EXPECT_EQ(cu->shippedLibDescriptors().size(), 6u);
    assertNoBuilderErrors(*cu);

    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "all five descriptor symbols must resolve";
    // Five distinct symbols, each minted EXACTLY once (dup not doubled).
    EXPECT_EQ(countSymbolsNamed(model, "dup"), 1u)
        << "dup included twice is minted ONCE — not per directive";
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(countSymbolsNamed(model, "s" + std::to_string(i)), 1u);
    }
    EXPECT_EQ(model.shippedExterns().size(), 5u)
        << "five distinct descriptor symbols → five extern rows (no duplicate)";
}

// ── FC2: explicit C-style casts (`semantics.casts`) ─────────────────────

// THE Part-B integration point: the IMPLICIT F64→I32 narrowing in
// `return 1.7 + 2.5;` is rejected (S_ReturnTypeMismatch — the strict
// no-silent-conversion bar), while the EXPLICIT `(int)(1.7 + 2.5)` form
// is accepted: the cast node's result type is the stamped target (I32),
// which assigns cleanly into main's I32 result. Both directions in one
// test so the contrast is pinned, not assumed.
TEST(SemanticAnalyzerCSubset, ExplicitFloatToIntCastAcceptedWhereImplicitRejected) {
    auto implicitModel = analyzeShipped("c-subset", {
        "int main() { return 1.7 + 2.5; }\n",
    });
    EXPECT_EQ(countCode(implicitModel.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 1u)
        << "the implicit F64->I32 narrowing must stay rejected";

    auto castModel = analyzeShipped("c-subset", {
        "int main() { return (int)(1.7 + 2.5); }\n",
    });
    EXPECT_FALSE(castModel.hasErrors())
        << (castModel.diagnostics().all().empty()
                ? ""
                : castModel.diagnostics().all()[0].actual);
    EXPECT_EQ(countCode(castModel.diagnostics(),
                        DiagnosticCode::S_InvalidCast), 0u);
}

// A typedef name in cast position resolves through the SAME type-position
// resolver declarations use (SE5 alias resolution) — `(T)4` yields I64,
// then `(int)` narrows it back; both casts legal, zero diagnostics.
TEST(SemanticAnalyzerCSubset, TypedefNameInCastPositionResolves) {
    auto model = analyzeShipped("c-subset", {
        "typedef long T;\n"
        "int main() { return (int)(T)4; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
}

// An unknown type name inside a COMMITTED cast (`(q) z` — the non-
// operator follower made the cast the only viable parse) fails loud at
// type resolution: exactly one S_UnknownType for `q`. (`z` additionally
// fails name resolution — a distinct code, deliberately not conflated.)
TEST(SemanticAnalyzerCSubset, UnknownTypeNameInCommittedCastFiresUnknownType) {
    auto model = analyzeShipped("c-subset", {
        "int main() { (q) z; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 1u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UndeclaredIdentifier), 1u)
        << "the operand `z` is an ordinary undeclared value reference";
}

// C forbids casts to (and the MIR lattice cannot lower casts from)
// composite VALUES: both the struct→int and int… to-struct directions
// emit S_InvalidCast — one per illegal cast site, nothing silent.
TEST(SemanticAnalyzerCSubset, StructValueCastsAreRejected) {
    auto model = analyzeShipped("c-subset", {
        "struct S { int x; };\n"
        "int main() { struct S s; int y = (int)s; (struct S)s; return y; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidCast), 2u)
        << "struct->int AND ->struct directions must BOTH fail loud";
}

// Pointer casts: ptr↔ptr, int→ptr (the null-constant idiom and beyond),
// and ptr→int are all in the explicit-cast matrix (mapCast: Bitcast /
// IntToPtr / PtrToInt). Zero diagnostics.
TEST(SemanticAnalyzerCSubset, PointerCastsAccepted) {
    auto model = analyzeShipped("c-subset", {
        "int main() {\n"
        "  int* p; void* v;\n"
        "  p = (int*)0;\n"
        "  v = (void*)p;\n"
        "  p = (int*)v;\n"
        "  long bits = (long)p;\n"
        "  return (int)bits;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
}

// Float↔pointer stays ILLEGAL (a C constraint mapCast mirrors: no
// FPToPtr arm exists) — S_InvalidCast, never a silent miscompile.
TEST(SemanticAnalyzerCSubset, FloatToPointerCastRejected) {
    auto model = analyzeShipped("c-subset", {
        "int main() { int* p; p = (int*)1.5; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidCast), 1u);
}
