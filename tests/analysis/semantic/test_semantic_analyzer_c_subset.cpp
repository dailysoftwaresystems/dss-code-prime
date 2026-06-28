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
    // main (function) + x (variable) + the 2 FC12a-core builtin TYPES
    // (`__va_list_tag` + `va_list`) injected into every c-subset CU's builtin scope
    // (D-FC12A-VARIADIC-CALLEE — gated on the schema declaring `vaArgRule`).
    ASSERT_EQ(model.symbols().size() - 1, 4u)
        << "main (function) + x (variable) + __va_list_tag + va_list";
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

// ── c34 (D-CSUBSET-ARRAY-SIZE-INFERENCE, C 6.7.9p22) ────────────────────────
// A `[]` (empty-bound) array WITH an initializer infers its length from the
// initializer: a string literal → decoded-bytes + 1 (the NUL); a brace list →
// the top-level element count. The completion happens ONCE in the semantic model
// (Pass 1.5), so the SYMBOL's resolved type is the sized array every downstream
// tier observes. These pins assert the symbol's `.type` directly (red-on-disable:
// without the completion the type stays an incomplete array — scalars()[0] is the
// kIncompleteArrayLength sentinel, not N).

[[nodiscard]] inline SymbolRecord const*
findSym(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return &m.symbols()[i];
    return nullptr;
}

// (1) `char x[] = "abc"` resolves to a 4-element char array ("abc" + NUL).
TEST(SemanticAnalyzerCSubset, ArraySizeInferredFromStringInit) {
    auto cu = buildShippedUnit("c-subset", { "char x[] = \"abc\";\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* x = findSym(model, "x");
    ASSERT_NE(x, nullptr);
    ASSERT_TRUE(x->type.valid());
    ASSERT_EQ(ti.kind(x->type), TypeKind::Array);
    ASSERT_FALSE(ti.isIncompleteArray(x->type));
    ASSERT_EQ(ti.scalars(x->type).size(), 1u);
    EXPECT_EQ(ti.scalars(x->type)[0], 4);   // 'a' 'b' 'c' '\0'
}

// (2) `int a[] = {1,2,3}` → Array<I32, 3> (top-level brace element count).
TEST(SemanticAnalyzerCSubset, ArraySizeInferredFromBraceInit) {
    auto cu = buildShippedUnit("c-subset", { "int a[] = {1, 2, 3};\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* a = findSym(model, "a");
    ASSERT_NE(a, nullptr);
    ASSERT_TRUE(a->type.valid());
    ASSERT_EQ(ti.kind(a->type), TypeKind::Array);
    ASSERT_FALSE(ti.isIncompleteArray(a->type));
    EXPECT_EQ(ti.scalars(a->type)[0], 3);
    EXPECT_EQ(ti.kind(ti.operands(a->type)[0]), TypeKind::I32);
}

// (3) LOCAL variant — block-scope `[]`-with-init infers at the SAME Pass-1.5 site
// (the local var path shares `resolveDeclTypes`, not a separate completion).
TEST(SemanticAnalyzerCSubset, ArraySizeInferredFromInitLocal) {
    auto cu = buildShippedUnit("c-subset",
                               { "int main(void){ int a[] = {10, 20, 30}; return a[2]; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* a = findSym(model, "a");
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(ti.kind(a->type), TypeKind::Array);
    ASSERT_FALSE(ti.isIncompleteArray(a->type));
    EXPECT_EQ(ti.scalars(a->type)[0], 3);
}

// (4) PRESERVE — an EXPLICIT `[N]` is unchanged by the inference path (the
// resolved length folds normally; completion is a no-op on an already-sized
// array). `char x[4] = "abc"` stays Array<Char,4>, NOT re-derived to 4-from-init.
TEST(SemanticAnalyzerCSubset, ExplicitArraySizeUnchangedByInference) {
    auto cu = buildShippedUnit("c-subset",
                               { "int a[3] = {1, 2, 3}; char x[8] = \"abc\";\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* a = findSym(model, "a");
    SymbolRecord const* x = findSym(model, "x");
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(ti.kind(a->type), TypeKind::Array);
    EXPECT_EQ(ti.scalars(a->type)[0], 3);
    ASSERT_NE(x, nullptr);
    ASSERT_EQ(ti.kind(x->type), TypeKind::Array);
    EXPECT_EQ(ti.scalars(x->type)[0], 8)   // the DECLARED 8, not "abc"+NUL == 4
        << "an explicit [N] must keep N — inference only fills an empty []";
}

// (5) A `[]` with NO initializer is NOT silently sized — the resolver's
// S_NonConstantArrayLength still fires (inference is gated on an initializer
// being present, so a bare `int x[];` is unaffected by c34).
TEST(SemanticAnalyzerCSubset, EmptyArrayNoInitNotSized) {
    auto cu = buildShippedUnit("c-subset", { "int main(void){ int a[]; return 0; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 1u);
    // The symbol's type must NOT be a sized array (it stays unresolved/incomplete).
    auto const& ti = model.lattice().interner();
    SymbolRecord const* a = findSym(model, "a");
    if (a != nullptr && a->type.valid() && ti.kind(a->type) == TypeKind::Array) {
        EXPECT_TRUE(ti.isIncompleteArray(a->type))
            << "a no-init [] must never be silently completed to a sized array";
    }
}

// c34 fail-loud (audit-caught regression): an EMPTY-brace inferred array
// `int a[] = {}` cannot determine a positive length, so it must FAIL LOUD — NOT
// leave a silently-incomplete array type that flows into the unguarded HIR/MIR
// tier and LOOPS on the -1 sentinel length (a compiler HANG). An inferred 0/
// undeterminable length is the non-positive `int a[0]` case → S_ArrayLengthOutOfRange.
// RED-ON-DISABLE: revert the `failUnsized` guard (empty-brace returns the
// incomplete array with no diagnostic) → this flips AND a CLI compile hangs.
TEST(SemanticAnalyzerCSubset, ArraySizeInferenceEmptyBraceFailsLoud) {
    auto cu = buildShippedUnit("c-subset", { "int a[] = {};\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_TRUE(model.hasErrors())
        << "`int a[] = {}` cannot infer a positive size — must fail loud, not hang";
    EXPECT_GT(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayLengthOutOfRange), 0u)
        << "empty-brace inferred array → S_ArrayLengthOutOfRange (inferred 0-length)";
    auto const& ti = model.lattice().interner();
    SymbolRecord const* a = findSym(model, "a");
    if (a != nullptr && a->type.valid() && ti.kind(a->type) == TypeKind::Array) {
        EXPECT_TRUE(ti.isIncompleteArray(a->type))
            << "an un-sizable [] must never be a usable sized array";
    }
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

// D-TYPEINTERNER-OPERAND-SPAN-LIFETIME-GUARD regression (red-on-disable on DEBUG):
// `checkCallAgainstSig` held the callee's `fnParams()` span across the per-arg
// `subtreeType()` loop. An `&x` argument MATERIALIZES `pointer<int>` on first use,
// mutating the interner pool MID-LOOP, so the retained `params` span dangled — a
// heap-use-after-free masked in Release (the guard is compiled out → exit 42 by
// luck) and caught only on Debug. This is the `memcpy(&b,&a,4)` (`#include
// <string.h>`) case that read "libc FFI 9/10" on Debug. A MULTI-param callee + an
// address-of arg is the minimal trip: the FIRST `&x` interns pointer<int>, then
// `params[1]`/`params[2]` read the now-stale span. Single-param libc fns
// (malloc/free) never trip it — a literal `4` / an existing pointer arg interns
// nothing. The fix copies `params` into an owned vector before the loop; WITHOUT
// it, this `analyze()` ABORTS (the guard) on a Debug build → the test goes red.
TEST(SemanticAnalyzerCSubset, MultiParamCallAddressOfArgsNoStaleParamSpan) {
    // The callee params are `void*` (NOT `int*`) — this is load-bearing for the
    // red-on-disable. The bug needs the arg's `subtreeType()` to intern a FRESH
    // type mid-loop: `&x` is `int*`, which is NOT already interned (the params are
    // `void*`), so checking it materializes pointer<int> and mutates the pool —
    // exactly memcpy's `void*` params + `&b`/`&a` `int*` args. (An `int*`-param
    // version does NOT trip it: `&x` dedups against the param's pointer<int>, no
    // mutation.) `int*` → `void*` is implicit in c-subset, so the call is
    // well-typed; WITHOUT the owned-copy fix this analyze() aborts (guard) on Debug.
    auto cu = buildShippedUnit("c-subset", {
        "void multi(void* a, void* b, int n);\n"
        "void f(void) {\n"
        "    int x;\n"
        "    int y;\n"
        "    multi(&x, &y, 4);\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // The call is well-typed (int*→void* implicit, int→int): no mismatch, no abort.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArgCountMismatch), 0u);
    EXPECT_FALSE(model.hasErrors());
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

// ── D-SEMANTIC-ASSIGN-STMT-ASSIGNABILITY-BYPASS — the assignment STATEMENT
//    now runs the SAME `isAssignable` check as the init/call-arg/return sites ──
//
// (a) An invalid assignment STATEMENT `x = f;` (int <- float) fails loud with a
// positioned S_TypeMismatch — the SAME diagnostic the init site `int x = f;`
// emits. `f` is a parameter so no narrowing initializer adds a second mismatch;
// exactly ONE fires.
// RED-ON-DISABLE: remove the assignment-statement isAssignable arm (restore the
// bypass) -> the assignment is silently accepted (HIR coerce truncates float ->
// int), this count drops to 0.
TEST(SemanticAnalyzerCSubset, AssignStmtIntFromFloatFailsLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "int sink(float f) { int x; x = f; return x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "an int <- float assignment STATEMENT must fail loud with the same "
           "S_TypeMismatch the int <- float INIT (`int x = f;`) emits — the "
           "assignment-statement assignability bypass is closed";
}

// (b) PARITY pin: the init form `int x = f;` and the statement form `x = f;`
// must behave IDENTICALLY (both reject the same incompatible pair). Reading both
// in one TU yields exactly TWO S_TypeMismatch — one per site — proving the
// statement is no longer the lone unchecked position.
// RED-ON-DISABLE: with the bypass restored only the INIT fires -> count is 1.
TEST(SemanticAnalyzerCSubset, AssignStmtAndInitRejectIncompatibleIdentically) {
    auto cu = buildShippedUnit("c-subset", {
        "int sink(float f) { int x = f; x = f; return x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 2u)
        << "the init site AND the assignment-statement site must each reject the "
           "int <- float pair — two positioned S_TypeMismatch, not one";
}

// (c) A VALID assignment statement stays byte-identically clean: int <- int,
// pointer <- null-constant, and a cross-signedness assignment (the c-subset
// `intCrossSignednessConverts` gate is ON) all pass with ZERO diagnostics. The
// new arm must not over-reject any conversion the four checked sites admit.
TEST(SemanticAnalyzerCSubset, ValidAssignStmtsRemainClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(void) {\n"
        "  int x; int y; unsigned u; int* p; int a;\n"
        "  y = 7;\n"      // int <- int
        "  x = y;\n"      // int <- int
        "  u = y;\n"      // unsigned <- int (cross-signedness, gated ON)
        "  p = 0;\n"      // ptr <- null pointer constant
        "  p = &a;\n"     // ptr <- &lvalue (same typed pointer)
        "  return x;\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "every valid assignment statement (int<-int, unsigned<-int gated, "
           "ptr<-null, ptr<-&lvalue) must stay accepted — the new check admits "
           "exactly what the init/call-arg/return sites admit";
    EXPECT_FALSE(model.hasErrors());
}

// (c2) Valid assignment statements to NON-trivial LVALUES — a DEREF store
// (`*p = v`), an ARRAY-ELEMENT store (`a[i] = v`), and a MEMBER store (`s.m = v`) —
// stay byte-identically CLEAN (zero S_TypeMismatch). The assignability check reads
// the LHS via subtreeType, which returns the lvalue's VALUE type for a deref /
// index / member-access, so each compatible store is admitted. This guards the
// lvalue-shaped LHS forms the plain-variable cases above do not exercise.
TEST(SemanticAnalyzerCSubset, ValidLvalueStoreAssignStmtsRemainClean) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int m; };\n"
        "int main(void) {\n"
        "  int a[4]; int x; int* p; struct S s;\n"
        "  p = &x;\n"
        "  *p = 5;\n"        // deref store: int <- int
        "  a[2] = 7;\n"      // array-element store: int <- int
        "  s.m = 9;\n"       // member store: int <- int
        "  return a[2] + s.m + *p;\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "a deref store (*p=v), an array-element store (a[i]=v), and a member "
           "store (s.m=v) of a compatible value must each stay clean — subtreeType "
           "returns the lvalue's value type for the assignability check";
    EXPECT_FALSE(model.hasErrors());
}

// (d) A COMPOUND assignment is NOT routed through the plain-assignment check:
// `x += y` is `x = x + y` whose result is the arithmetic common type converted
// back to x (the usual-arithmetic path, not assignability). The plain-vs-compound
// discriminator is the operator-table entry's `target == "Assign"` (config-driven,
// the same one subtreeType uses), so a compound assignment of two ints raises NO
// spurious S_TypeMismatch here. (c-subset does not yet LOWER compound-assign, but
// the SEMANTIC tier must not mis-reject it.)
TEST(SemanticAnalyzerCSubset, CompoundAssignStmtNotCheckedAsPlainAssign) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(void) { int x; int y; y = 1; x = 0; x += y; return x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "a compound assignment (`x += y`) must not run the plain-assignment "
           "assignability check — only the plain `=` operator is checked";
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

// R2 (D-SEMANTIC-NULL-CONSTANT-FOLDING ✅ CLOSED) — F3 PIN FLIP: `f(-0)`. `-0` is a
// non-literal integer constant expression that FOLDS to 0, so per C §6.3.2.3p3 it
// IS a null pointer constant and admits to `void*` WITHOUT a mismatch. This pin was
// the INVERSE before R2 (NegativeZeroAtVoidPtrArgFiresMismatch): the literal-only
// `isLiteralIntegerZero` rejected `-0` → S_TypeMismatch and the test asserted that
// reject. R2's folded path admits it. RED-ON-DISABLE: revert the const-fold path in
// `admitsNullPointerConstant` and this flips back to 1× S_TypeMismatch. The
// integer-kind + folds-to-0 gates keep `f(1)` / `f(1+1)` / `f(1.5-1.5)` rejected
// (their own pins, above and below).
TEST(SemanticAnalyzerCSubset, NegativeZeroAdmitsAsNullPointerConstant) {
    auto cu = buildShippedUnit("c-subset", {
        "extern void f(void* p);\n"
        "int main() { f(-0); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "`-0` folds to integer 0 → a null pointer constant (C §6.3.2.3p3)";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u);
}

// R2: the folded-zero null-pointer admit fires at ALL THREE conversion contexts —
// call-arg, return, and initializer (the shared `admitsNullPointerConstant` site,
// reached from checkCallAgainstSig, checkReturn, and pass-2 decl-init). `1 - 1` /
// `2 - 2` are non-literal integer constant expressions folding to 0.
TEST(SemanticAnalyzerCSubset, FoldedZeroAdmitsAsPointerArg) {
    auto cu = buildShippedUnit("c-subset", {
        "extern void f(void* p);\n"
        "int main() { f(1 - 1); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "`1 - 1` folds to 0 → null pointer constant at a call arg";
}

TEST(SemanticAnalyzerCSubset, FoldedZeroAdmitsAsReturn) {
    auto cu = buildShippedUnit("c-subset", {
        "int* g() { return 1 - 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u)
        << "`return 1 - 1;` from an int*-returning function is a null constant";
}

TEST(SemanticAnalyzerCSubset, FoldedZeroAdmitsAsInit) {
    auto cu = buildShippedUnit("c-subset", {
        "void f() { int* p = 2 - 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "`int* p = 2 - 2;` initializer is a null pointer constant";
}

// R2 negative: a FLOAT zero (`1.5 - 1.5`) is NOT a null pointer constant — C
// §6.3.2.3p3 requires an INTEGER constant expression — so it is rejected
// (S_TypeMismatch). HONEST NOTE (self-audit Finding 2): two INDEPENDENT guards
// reject it — (a) the integer-kind gate (`subtreeType` types `1.5-1.5` as F64,
// failing the signed/unsigned int-rank check) AND (b) a backstop: `constIntExpr`
// returns nullopt for any float (`asInt64` has no `double` arm). So this pins the
// BEHAVIOR (float-zero rejects), NOT the gate in isolation — removing the gate
// leaves it green via the const-fold backstop. The gate is defense-in-depth that
// additionally excludes a Char/Bool-typed fold the const-fold step would otherwise
// fold to 0 (consistent with DSS typing comparisons as Bool, not C's int).
TEST(SemanticAnalyzerCSubset, FloatZeroRejectsAsPointerArg) {
    auto cu = buildShippedUnit("c-subset", {
        "extern void f(void* p);\n"
        "int main() { f(1.5 - 1.5); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "a float zero is NOT a null pointer constant — integer constant "
           "expression required";
}

// R2 (self-audit Finding 1 guard): the folded-null marker is a TREE-KEYED
// UnitAttribute, so a MULTI-SOURCE CU — where each tree restarts NodeId numbering
// at 1 — routes the mark per-tree and cannot alias node K across files. A flat
// NodeId.v set (the bug this replaced) would have risked falsely marking the other
// tree's same-index node → a silent miscompile at HIR lowering. This exercises the
// multi-tree set/route path (no other test compiles >1 tree through the marker);
// the cross-tree QUERY correctness is by-construction (UnitAttribute is the exact
// per-tree mechanism nodeToType/nodeToSymbol use). Both folded-zero nulls admit,
// no cross-contamination.
TEST(SemanticAnalyzerCSubset, FoldedNullMarkerIsTreeKeyedAcrossSources) {
    auto cu = buildShippedUnit("c-subset", {
        "int* a() { return 1 - 1; }\n",
        "int* b() { return 2 - 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "folded-zero null constants in two trees of one CU both admit, "
           "tree-keyed marker → no cross-tree contamination";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u);
}

// The latent-bug FIX (D-SEMANTIC-SUBTREETYPE-TRANSPARENT-WRAPPERS closure): a
// mixed-type binary in a checked position is typed against its UNIFIED (UAC)
// type, not whichever leaf the old DFS-suppressor happened to reach. The
// observable uses a FLOAT operand: D-CSUBSET-INT-SAME-SIGN-NARROW made integer
// narrowing implicit (so the old long+int→int observable no longer fires), but
// int↔float stays NON-implicit. For `sink(int); double a; int b; sink(a + b)`
// the argument `a + b` is `double` (UAC of double+int) and double→int is NOT
// assignable → S_TypeMismatch fires. Under the old suppressor it reached the
// `int` leaf `b` and silently admitted. RED-ON-DISABLE: revert the binary arm to
// a leaf type and this drops to 0 (the latent unsoundness this closure removes).
TEST(SemanticAnalyzerCSubset, MixedWidthBinaryArgTypedByUacNotLeaf) {
    auto cu = buildShippedUnit("c-subset", {
        "extern int sink(int v);\n"
        "int f(double a, int b) { return sink(a + b); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // `a + b` is `double` (UAC); the `int` param cannot take a float → one mismatch.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u);
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

// R1 (D-EXPRTYPE-PASS15-FORWARD-REF member-access case ✅ CLOSED): a member access
// in a Pass-1.5 const context — `int a[sizeof(s.y)]` — types the member via the
// shared `resolveMemberAccess`, so the array dimension FOLDS (sizeof(int)=4)
// instead of failing loud spuriously. Strong pin: `a` is Array<I32, 4>.
// Red-on-disable: revert subtreeType's member arm (→ `return InvalidType` for the
// member verb) → `s.y` is InvalidType at Pass 1.5 → the dim can't fold → `a` is
// unresolved AND S_NonConstantArrayLength fires (BOTH asserts flip). NOTE: the
// array's runtime readback (indexing / sizeof-value-of-a-local) is blocked by
// independent gaps (FC7 local-array indexing; sizeof-VALUE-of-local), so R1 is a
// semantic-tier feature proven HERE (the §A.5 carve-out), not via a runtime corpus.
TEST(SemanticAnalyzerCSubset, MemberAccessSizeofResolvesArrayDimension) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int x; int y; };\n"
        "int main() { struct S s; int a[sizeof(s.y)]; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    // aggregateLayout MUST be present for an array-dim sizeof to fold at all
    // (nullopt ⇒ deliberate fail-loud). The scalar `int` size (4) is dataModel-
    // driven, independent of these alignment params.
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u)
        << "sizeof(s.y) must fold the member size — no spurious fail-loud";
    auto const& ti = model.lattice().interner();
    SymbolRecord const* aRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "a") aRec = &model.symbols()[i];
    ASSERT_NE(aRec, nullptr);
    ASSERT_TRUE(aRec->type.valid());
    ASSERT_EQ(ti.kind(aRec->type), TypeKind::Array);
    ASSERT_EQ(ti.scalars(aRec->type).size(), 1u);
    EXPECT_EQ(ti.scalars(aRec->type)[0], 4)
        << "dimension = sizeof(int) = 4 (member s.y resolved to int)";
}

// R1: the GENUINE forward-reference case (`int a[sizeof(b)]; int b;` — b used
// before its declaration's type resolves) STAYS correct fail-loud (invalid C:
// declare-before-use). This is NOT the closed member-access case — it pins the
// reclassified anchor: forward-ref rejected, member-access-at-Pass-1.5 closed.
TEST(SemanticAnalyzerCSubset, ForwardRefSizeofArrayDimensionStillRejected) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { int a[sizeof(b)]; int b; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 1u)
        << "a forward-referenced sizeof operand must fail loud, never fold";
}

// R1: a non-existent field in the Pass-1.5 const context fails loud (the helper's
// UndeclaredField → the dim cannot fold). Guards against the member arm admitting
// a phantom field and folding a guessed size.
TEST(SemanticAnalyzerCSubset, BadFieldSizeofArrayDimensionRejected) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int x; int y; };\n"
        "int main() { struct S s; int a[sizeof(s.nope)]; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 1u)
        << "sizeof(s.nope) — no such field — must fail loud, never fold a guess";
}

// FC12b (D-FC12B-WIN64-VARIADIC-CALLEE, BLOCKER-2) sizeof(va_list) pin: the injected
// `va_list` TYPE is strategy-selected, so its size differs per ABI — 24B under SysV
// (`__va_list_tag[1]` = {u32,u32,void*,void*}) vs 8B under Win64 (`char*`). A wrong
// size mis-sizes the `ap` local → stack corruption. Fold sizeof(va_list) into an
// array dimension (the established sizeof-folding probe) and read it back. RED-ON-
// DISABLE: a regression injecting the SysV tag under Win64 (or vice versa) flips the
// dimension.
TEST(SemanticAnalyzerCSubset, SizeofVaListIs24UnderSysV) {
    auto cu = buildShippedUnit("c-subset", {
        "int a[sizeof(va_list)];\n",
    });
    assertNoBuilderErrors(*cu);
    // SysVRegisterSave (the default/absent strategy): va_list = __va_list_tag[1].
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                         VaListStrategy::SysVRegisterSave);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* aRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "a") aRec = &model.symbols()[i];
    ASSERT_NE(aRec, nullptr);
    ASSERT_TRUE(aRec->type.valid());
    ASSERT_EQ(ti.kind(aRec->type), TypeKind::Array);
    ASSERT_EQ(ti.scalars(aRec->type).size(), 1u);
    EXPECT_EQ(ti.scalars(aRec->type)[0], 24)
        << "sizeof(va_list) under SysV = sizeof(__va_list_tag[1]) = 24";
}

TEST(SemanticAnalyzerCSubset, SizeofVaListIs8UnderWin64) {
    auto cu = buildShippedUnit("c-subset", {
        "int a[sizeof(va_list)];\n",
    });
    assertNoBuilderErrors(*cu);
    // HomogeneousPointer (Win64): va_list = char* (one pointer = 8B).
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                         VaListStrategy::HomogeneousPointer);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* aRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "a") aRec = &model.symbols()[i];
    ASSERT_NE(aRec, nullptr);
    ASSERT_TRUE(aRec->type.valid());
    ASSERT_EQ(ti.kind(aRec->type), TypeKind::Array);
    ASSERT_EQ(ti.scalars(aRec->type).size(), 1u);
    EXPECT_EQ(ti.scalars(aRec->type)[0], 8)
        << "sizeof(va_list) under Win64 = sizeof(char*) = 8";
}

// FC12c (D-FC12C-AAPCS64-VARIADIC-CALLEE) sizeof(va_list) pin: AAPCS64 realizes the
// dual-cursor strategy by injecting `va_list = __va_list` (the 5-field struct
// {void* __stack; void* __gr_top; void* __vr_top; int __gr_offs; int __vr_offs;}) —
// 24B of pointers + 8B of i32 cursors = 32B under natural alignment. NOT an array
// (SysV), NOT a pointer (Win64) — the struct DIRECTLY. RED-ON-DISABLE: reverting to
// the FC12b fail-loud, or injecting a pointer/array shape, flips the dimension off 32.
TEST(SemanticAnalyzerCSubset, SizeofVaListIs32UnderAapcs64) {
    auto cu = buildShippedUnit("c-subset", {
        "int a[sizeof(va_list)];\n",
    });
    assertNoBuilderErrors(*cu);
    // Aapcs64DualCursor: va_list = __va_list (the 5-field struct, 32B).
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                         VaListStrategy::Aapcs64DualCursor);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_VariadicCalleeUnsupported), 0u)
        << "AAPCS64 va_list is realized in FC12c — no fail-loud at injection";
    auto const& ti = model.lattice().interner();
    SymbolRecord const* aRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "a") aRec = &model.symbols()[i];
    ASSERT_NE(aRec, nullptr);
    ASSERT_TRUE(aRec->type.valid());
    ASSERT_EQ(ti.kind(aRec->type), TypeKind::Array);
    ASSERT_EQ(ti.scalars(aRec->type).size(), 1u);
    EXPECT_EQ(ti.scalars(aRec->type)[0], 32)
        << "sizeof(va_list) under AAPCS64 = sizeof(__va_list) = 3*8 + 2*4 = 32";
}

// FC12c: an AAPCS64 variadic callee that walks its varargs analyzes CLEANLY now that
// the dual-cursor seam is realized (the FC12b-era fail-loud is gone). The c-subset
// `vaArgRule` gates the injection; the body NAMES + USES va_list/va_start/va_arg.
TEST(SemanticAnalyzerCSubset, Aapcs64VariadicCalleeAnalyzesClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(int n, ...) { va_list ap; va_start(ap, n);"
        " int t = va_arg(ap, int); va_end(ap); return t; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                         VaListStrategy::Aapcs64DualCursor);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_VariadicCalleeUnsupported), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "the __va_list struct ap operand must pass isVaList for AAPCS64";
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
    // main (function) + two distinct `x` symbols (one per block scope) + the 2
    // FC12a-core builtin TYPES (__va_list_tag + va_list).
    EXPECT_EQ(model.symbols().size() - 1, 5u);
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

    // main (function) + x (variable) + the 2 FC12a-core builtin TYPES
    // (__va_list_tag + va_list). Find x by name.
    ASSERT_EQ(model.symbols().size() - 1, 4u);
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

// ===== c36 (D-CSUBSET-MUTABLE-POINTER-TO-CONST) =====
// `const` qualifies the type it directly modifies (C 6.7.3). For a pointer
// declarator the OBJECT is const iff the OUTERMOST (last source-order) pointer
// layer carries `* const` — a HEAD/pointee const (`const char *p`) leaves the
// pointer OBJECT mutable. The verdict is read from the declarator structure
// (declaratorObjectIsConst), NOT a coarse whole-decl const scan. Each form
// below is a red-on-disable pin: revert the fix and the GROUP-2/5/8/9 "clean"
// pins flip to a spurious S_ConstViolation.

// GROUP 2 — pointer-to-const: the pointer object is MUTABLE (the bug; was a
// spurious S_ConstViolation before c36). This is the sqlite `zFormat += 4`.
TEST(SemanticAnalyzerCSubset, MutablePointerToConstParamIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(const char *p){ p += 4; return (int)*p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
}
TEST(SemanticAnalyzerCSubset, MutablePointerToConstEastIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(char const *p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
}

// GROUP 3 — const POINTER: the object IS const → modifying it violates.
TEST(SemanticAnalyzerCSubset, ConstPointerParamEmitsConstViolation) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(char * const p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
}

// GROUP 4 — const pointer to const: object const → violates.
TEST(SemanticAnalyzerCSubset, ConstPointerToConstParamEmitsConstViolation) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(const char * const p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
}

// GROUP 5 — multi-level pointers: the OUTERMOST layer decides.
// `char * const *p` — inner pointer const, OUTER pointer mutable → clean.
TEST(SemanticAnalyzerCSubset, MultiLevelInnerConstOuterMutableIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(char * const *p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
}
// `char ** const p` — OUTER pointer const → violates.
TEST(SemanticAnalyzerCSubset, MultiLevelOuterConstEmitsConstViolation) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(char ** const p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
}
// `const char **p` — pointee const, both pointers mutable → clean.
TEST(SemanticAnalyzerCSubset, MultiLevelHeadConstIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(const char **p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
}

// GROUP 8 — multi-declarator: each declarator's OWN outermost layer decides.
// `const int *p, x;` → p is pointer-to-const (mutable), x is a const scalar.
TEST(SemanticAnalyzerCSubset, MultiDeclaratorPointerCleanScalarViolates) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(){ const int *p, x; p += 1; x = 2; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // exactly one violation — on `x` (the const scalar), NOT on `p`.
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_ConstViolation) EXPECT_EQ(d.actual, "x");
    }
}

// GROUP 9 — const + volatile together must NOT regress c27.
// `volatile char * const p` — const POINTER (volatile pointee) → violates.
TEST(SemanticAnalyzerCSubset, VolatilePointeeConstPointerEmitsConstViolation) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(volatile char * const p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
}
// `const volatile char *p` — cv POINTEE, pointer object mutable → clean.
TEST(SemanticAnalyzerCSubset, ConstVolatilePointeeMutablePointerIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(const volatile char *p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
}

// GROUP 1 — scalar east-const still violates (no-pointer path unchanged).
TEST(SemanticAnalyzerCSubset, EastConstScalarStillEmitsConstViolation) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { int const x = 1; x = 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
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
    // The span points at `unused`'s declaration (the varDecl node).
    // Layout: "int main(){ int unused; int used=1; return used; }"
    //          0123456789012345678901234567890
    // FC4 c1: the declaration statement rule is `varDecl` (which INCLUDES
    // the terminating `;` — pre-FC4 the anchor was the semicolon-less
    // varDeclHead): `int unused;` spans 12..23 half-open. Pin both ends so
    // a regression that drifts the emit point off the decl node is loud.
    EXPECT_EQ(d->span.start(), 12u);
    EXPECT_EQ(d->span.end(),   23u);
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

// char→int WIDENING is legal C (C 6.3.1.1: `char` is an integer type) — `int f(char
// c){ return c; }` returns the char param widened to int. The BIDIRECTIONAL
// `charConvertsToArith` arm (D-CSUBSET-CHAR-INT-WIDENING ✅) admits it; codegen
// materializes the Char→int SExt (witnessed by the `char_value` corpus). This pin was
// the INVERSE before (ReturnTypeMismatchOnNonAssignable — DSS's earlier strict-char
// choice). RED-ON-DISABLE: revert the char→int arm direction → `isAssignable(I32,
// Char)` is false → 1 S_ReturnTypeMismatch. (A genuinely non-assignable return still
// fires — DistinctTypedReturnRemainsMismatch covers the mismatch mechanism.)
TEST(SemanticAnalyzerCSubset, CharParamReturnedAsIntIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(char c) { return c; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 0u)
        << "char widens to int on return (C 6.3.1.1) — the bidirectional char arm";
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

// char→int WIDENING through a CALL result: `char h()` returned from an `int`
// function `g` — h's Char result widens to int (C 6.3.1.1), now clean via the
// bidirectional char arm. (Was the inverse: ReturnOfMismatchedCallResultEmitsMismatch.)
// RED-ON-DISABLE: revert the char→int arm direction → 1 S_ReturnTypeMismatch. The
// genuine call-result mismatch mechanism is covered by DistinctTypedReturnRemainsMismatch.
TEST(SemanticAnalyzerCSubset, CharResultCallReturnedAsIntIsClean) {
    auto cu = buildShippedUnit("c-subset", {
        "char h() { return 'a'; }\n"
        "int g() { return h(); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 0u)
        << "a Char-result call widens to int on return (C 6.3.1.1)";
}

// R2 (sizeof char/string fold cycle): a CHARACTER constant has type `int`
// (C 6.4.4.4 — the reason `sizeof('c')`==4, not 1). Pinned in a context where the
// int type MATTERS: `f('c')` to an `int*` param fires a mismatch (int 99 is not a
// pointer, and not the null constant 0). RED-ON-DISABLE: drop the CharLiteral→I32
// `literalTypes` row → `'c'` is untyped → `isAssignable` short-circuits on
// InvalidType → 0 mismatch (the literal would silently pass).
TEST(SemanticAnalyzerCSubset, CharLiteralIsTypedIntNotUntyped) {
    auto cu = buildShippedUnit("c-subset", {
        "extern void f(int* p);\n"
        "int main() { f('c'); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
        << "'c' has type int (C 6.4.4.4) → passing it to an int* param is a mismatch";
}

// R2: a STRING literal has type `char[N+1]` (C 6.4.5 — the reason `sizeof("abcd")`
// ==5). Pinned where the ELEMENT type matters: passing "abc" to an `int*` param
// fires a mismatch (Array<Char>→Ptr<int> fails the same-element-type array-decay
// rule), while to a `char*` param it decays cleanly (0 — covered by the existing
// string corpus). RED-ON-DISABLE: drop the StringLiteral `stringArray` row → "abc"
// is untyped → `isAssignable` short-circuits → 0 mismatch.
TEST(SemanticAnalyzerCSubset, StringLiteralIsTypedCharArrayNotUntyped) {
    auto cu = buildShippedUnit("c-subset", {
        "extern void g(int* p);\n"
        "int main() { g(\"abc\"); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
        << "\"abc\" is Array<Char,4> → an int* arg is an element mismatch (Char != int)";
}

// C 5.1.1.2 phase 6 (D-CSUBSET-ADJACENT-STRING-CONCAT): adjacent string literals
// concatenate, and the WHOLE concatenated literal is typed `Array<Char, N+1>` on
// the stringLiteralExpr RULE node (N = sum of per-segment decoded lengths) — NOT
// per body token. `"hello" " world"` → 5 + 6 = 11 bytes + NUL = Array<Char,12>.
// The type is read directly off the rule node (`model.typeAt`), the same place
// `subtreeType` short-circuits on, so every downstream consumer sees the whole
// concatenated size. RED-ON-DISABLE: reading only the first body would stamp
// Array<Char,6> ("hello"+NUL); typing per-token (the pre-c20 shape) would leave
// the rule node untyped. Also pins that the body TOKENS are NOT individually
// typed (the restructure moved typing to the rule node).
TEST(SemanticAnalyzerCSubset, AdjacentStringConcatTypesWholeCharArray) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { \"hello\" \" world\"; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors());
    auto const& ti = model.lattice().interner();
    Tree const& tree = cu->trees()[0];
    RuleId const sleRule = tree.schema().rules().find("stringLiteralExpr");
    ASSERT_TRUE(sleRule.valid());

    NodeId sle{};
    int sleCount = 0;
    SchemaTokenId const bodyTok = tree.schema().schemaTokens().find("StringLiteral");
    int typedBodyTokens = 0;
    walkPreOrder(tree, [&](TreeCursor const& cursor) {
        NodeId const n = cursor.current();
        if (tree.kind(n) == NodeKind::Internal && tree.rule(n).v == sleRule.v) {
            sle = n; ++sleCount;
        }
        // A body TOKEN must NOT carry the per-token Array type any more — the
        // whole-literal type lives on the rule node.
        if (tree.kind(n) == NodeKind::Token && bodyTok.valid()
            && tree.tokenKind(n).v == bodyTok.v && model.typeAt(n).valid()) {
            ++typedBodyTokens;
        }
    });
    EXPECT_EQ(sleCount, 1) << "the two pieces form ONE stringLiteralExpr";
    ASSERT_TRUE(sle.valid());
    EXPECT_EQ(typedBodyTokens, 0)
        << "body tokens are no longer individually typed — the rule node carries "
           "the whole concatenated Array<Char,N>";

    TypeId const ty = model.typeAt(sle);
    ASSERT_TRUE(ty.valid()) << "the stringLiteralExpr rule node must be typed";
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    ASSERT_EQ(ti.scalars(ty).size(), 1u);
    EXPECT_EQ(ti.scalars(ty)[0], 12)
        << "\"hello\" \" world\" = \"hello world\" = 11 bytes + NUL = 12 "
           "(reading only the first piece would give 6)";
    ASSERT_EQ(ti.operands(ty).size(), 1u);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::Char);
}

// R2: the int→char assignability arm (`charConvertsToArith`, C 6.3.1.1). Typing the
// char literal `int` would otherwise REGRESS `char x = 'c';` (int → char slot, which
// DSS's strict lattice rejects without the arm). RED-ON-DISABLE: drop the arm →
// `isAssignable(Char, I32)` is false → 1 S_TypeMismatch. The arm is now BIDIRECTIONAL
// (D-CSUBSET-CHAR-INT-WIDENING ✅): the char→int direction is pinned clean by
// CharParamReturnedAsIntIsClean / CharResultCallReturnedAsIntIsClean above.
TEST(SemanticAnalyzerCSubset, CharLiteralInitializesCharSlotCleanly) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { char x = 'c'; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "an int literal initializing a char slot is legal C (the char↔int arm)";
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

// ── c32 D-CSUBSET-FNPTR-PARAM-SCOPE: per-declarator function-prototype scope ──
//
// The parameter NAMES of a function-POINTER declarator (and of a bare prototype)
// have function-prototype scope (C 6.2.1p4) — they terminate at the END of the
// declarator and must NOT bind into / collide across the enclosing scope. A
// function DEFINITION's params are EXEMPT (they bind into the definition's scope
// so they reach the body). Each pin below flips RED if the per-declarator
// prototype scope-open is reverted (the params would bind into the enclosing
// struct/file/block scope and collide).

// (1) fn-ptr STRUCT MEMBERS with a SHARED param name → no collision. This is the
// sqlite3_io_methods frontier (`int (*xRead)(…int iAmt…); int (*xWrite)(…int
// iAmt…);`). The two `iAmt`/`v` params live in DISTINCT prototype scopes.
TEST(SemanticAnalyzerCSubset, FnPtrStructMembersSharedParamNameNoRedecl) {
    auto model = analyzeShipped("c-subset", {
        "struct M { int (*a)(int v); int (*b)(int v); };\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "sibling fn-ptr members sharing a param name must not collide "
           "(per-declarator function-prototype scope)";
}

// (2) fn-ptr TYPEDEFS with a shared param name → no collision.
TEST(SemanticAnalyzerCSubset, FnPtrTypedefsSharedParamNameNoRedecl) {
    auto model = analyzeShipped("c-subset", {
        "typedef int (*A)(int x);\n"
        "typedef int (*B)(int x);\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "two fn-ptr typedefs sharing a param name must not collide";
}

// (3) fn-ptr PARAMS (of an ordinary function) with a shared param name → no
// collision. `void h(int (*a)(int x), int (*b)(int x));`
TEST(SemanticAnalyzerCSubset, FnPtrParamsSharedParamNameNoRedecl) {
    auto model = analyzeShipped("c-subset", {
        "void h(int (*a)(int x), int (*b)(int x));\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "fn-ptr parameters sharing a nested param name must not collide";
}

// (4) NON-LEAK: a fn-ptr's param name must NOT leak into the enclosing scope. A
// `typedef int (*A)(int gv);` followed by a GLOBAL `int gv;` must NOT collide
// (no S_RedeclaredSymbol), and the GLOBAL `gv` must remain the usable I32 symbol
// — the param `gv` neither shadowed nor clashed with it. (Revert the scope-open
// ⇒ the leaked param `gv` collides with the global ⇒ S_RedeclaredSymbol.)
TEST(SemanticAnalyzerCSubset, FnPtrParamNameDoesNotLeakToEnclosingScope) {
    auto model = analyzeShipped("c-subset", {
        "typedef int (*A)(int gv);\n"
        "int gv;\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "a fn-ptr typedef's param must not leak and collide with a later global";
    // The surviving `gv` is the GLOBAL int (typed I32), not the leaked param — a
    // leak would have made the param `gv` (a fn-prototype-scoped name) clash with
    // the global at file scope. Look it up directly (typeOfSymbol lives later in
    // this TU's anonymous namespace).
    auto const& in = model.lattice().interner();
    SymbolRecord const* gvRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "gv") gvRec = &model.symbols()[i];
    }
    ASSERT_NE(gvRec, nullptr) << "the global `gv` must exist and be usable";
    ASSERT_TRUE(gvRec->type.valid());
    EXPECT_EQ(in.kind(gvRec->type), TypeKind::I32)
        << "the surviving `gv` is the GLOBAL int, not the leaked param";
}

// (4b) ★ THE RESOLVE-FORM LEAK (decisive, distinct from the collision form above):
// a fn-ptr typedef's param name USED outside its declarator with NO same-named
// global must be UNDECLARED. A leak would make `gv` resolve to the
// prototype-scoped param — a SILENT correctness bug (no diagnostic at all), which
// the collision pin (both names I32) cannot detect. RED-ON-DISABLE: revert the
// per-declarator prototype scope-open → `gv` resolves to the leaked param → the
// S_UndeclaredIdentifier vanishes.
TEST(SemanticAnalyzerCSubset, FnPtrParamNameDoesNotResolveOutsideDeclarator) {
    auto model = analyzeShipped("c-subset", {
        "typedef int (*A)(int gv);\n"
        "int main(void){ return gv; }\n",
    });
    EXPECT_GT(countCode(model.diagnostics(),
                        DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "a fn-ptr param name must NOT resolve outside its declarator "
           "(C 6.2.1p4 function-prototype scope) — `gv` is undeclared here";
}

// (5) NESTED fn-ptr params: a fn-ptr whose own param is itself a fn-ptr with a
// shared inner param name, declared twice → no collision at any depth.
// `int (*a)(int (*p)(int q)); int (*b)(int (*p)(int q));`
TEST(SemanticAnalyzerCSubset, NestedFnPtrParamsSharedNamesNoRedecl) {
    auto model = analyzeShipped("c-subset", {
        "struct N { int (*a)(int (*p)(int q));\n"
        "           int (*b)(int (*p)(int q)); };\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "nested fn-ptr params (p/q) repeated across siblings must not collide";
}

// (6) ★ DEFINITION PARAMS STAY BODY-VISIBLE (the trap a wrong fix breaks). A
// function definition's own params bind into the definition's scope so the body
// resolves them; a NESTED definition taking two fn-ptr params with a shared inner
// param name (`e`) keeps cb/cb2 body-visible AND isolates the inner `e`s. Clean
// analysis (no undeclared `cb`/`cb2`, no redecl on `e`) is the witness.
TEST(SemanticAnalyzerCSubset, DefinitionParamsRemainBodyVisible) {
    auto model = analyzeShipped("c-subset", {
        "int run(int (*cb)(int e), int (*cb2)(int e)){ return cb(41)+cb2(0); }\n"
        "int g0(int e){return e+1;}\n"
        "int g1(int e){return e;}\n"
        "int main(void){ return run(g0,g1); }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "definition params must reach the body (cb/cb2 visible) while the "
           "fn-ptr params' inner `e`s stay isolated";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
}

// (7) PRESERVE: plain prototypes each with a param named `x` → clean (each
// prototype is a topLevelDecl scope, so the params already isolate; the c32 path
// adds a redundant-but-harmless prototype scope and must not change this).
TEST(SemanticAnalyzerCSubset, PlainPrototypesSharedParamNameClean) {
    auto model = analyzeShipped("c-subset", {
        "int f(int x);\n"
        "int g(int x);\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u);
}

// (8) PRESERVE the genuine-duplicate error: two params named `x` in ONE
// declarator still collide (they share the SAME prototype scope), in a fn-ptr
// member too — the isolation is PER-DECLARATOR, not per-param. (The function-
// DEFINITION form is pinned by DuplicateParamNamesEmitRedecl above.)
TEST(SemanticAnalyzerCSubset, DuplicateParamNameInSingleFnPtrStillCollides) {
    auto model = analyzeShipped("c-subset", {
        "struct M { int (*a)(int x, int x); };\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "a genuine duplicate within ONE declarator's param list must still error";
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

// ── GAP F (FC13): split `# include` ───────────────────────────────────────

// `# include "x.h"` (whitespace between `#` and `include`) is recognized by
// the config-selected preprocessor (its directive scan skips trivia between
// the intro `#` and the `include` word) and the header is INLINED. This both
// closes GAP F and proves the FC13 splice handles a spaced directive: one
// tree, zero cross-refs, header text present.
TEST(SemanticAnalyzerCSubset, SpacedIncludeIsInlined) {
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
    builder.addInMemory("# include \"x.h\"\nint main() { return helper(); }\n", "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    EXPECT_EQ(cu->trees().size(), 1u);
    EXPECT_TRUE(cu->crossRefs().empty());
    EXPECT_FALSE(hasCode(cu->driverDiagnostics(), DiagnosticCode::D_UnresolvedImport));
    EXPECT_NE(std::string{cu->trees()[0].source().text()}.find("int helper()"),
              std::string::npos)
        << "a spaced `# include` must still be recognized + inlined";
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

// ── Item 1: shipped-header CONSTANTS + TYPEDEFS via the neutral descriptor ────

// A shipped CONSTANT injects + folds in CONSTANT-EXPRESSION position (an array
// dimension) — the const-eval direct-value arm (MF-1). The descriptor CHAR_BIT
// (=8) makes `int a[CHAR_BIT]` a valid 8-element array. RED-ON-DISABLE: remove
// the const-eval `resolveSymbolValue` arm and `int a[CHAR_BIT]` fails loud with
// S_NonConstantArrayLength (an injected constant has no init-CST to walk).
TEST(SemanticAnalyzerCSubset, ShippedConstantFoldsInArrayDimension) {
    ScratchDir sysDir{Location::Temp, "item1-const"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "limits.json",
        R"({ "header": "limits.h",
             "constants": [ { "name": "CHAR_BIT", "value": 8, "type": "i32" } ] })",
        "#include <limits.h>\nint main() { int a[CHAR_BIT]; return 0; }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "CHAR_BIT must resolve to the injected descriptor constant";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NonConstantArrayLength), 0u)
        << "the injected constant must fold in array-dimension (const-expr) position";
    // The constant folded to the RIGHT value (8) — assert the resolved array
    // EXTENT, not merely the absence of a fail-loud (red-on-WRONG-value, not
    // just red-on-didn't-fold).
    auto const& ti = model.lattice().interner();
    SymbolRecord const* aRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "a") aRec = &model.symbols()[i];
    ASSERT_NE(aRec, nullptr);
    ASSERT_EQ(ti.kind(aRec->type), TypeKind::Array);
    EXPECT_EQ(ti.scalars(aRec->type)[0], 8)
        << "CHAR_BIT must fold to 8 in the array dimension";
}

// A shipped TYPEDEF injects as a Type symbol + resolves in TYPE position.
// RED-ON-DISABLE: skip the typedef injection loop and `my_int_t x;` fails loud
// with S_UnknownType.
TEST(SemanticAnalyzerCSubset, ShippedTypedefResolvesInTypePosition) {
    ScratchDir sysDir{Location::Temp, "item1-typedef"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "mytypes.json",
        R"({ "header": "mytypes.h",
             "typedefs": [ { "name": "my_int_t", "type": "i32" } ] })",
        "#include <mytypes.h>\nint main() { my_int_t x; x = 5; return x; }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u)
        << "my_int_t must resolve via the injected descriptor typedef";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
}

// GOAL-2: a user decl of a name WINS over a descriptor constant of the same
// name — and the skip is SELECTIVE (a different descriptor constant the user
// does NOT declare is still injected). The descriptor declares CHAR_BIT
// (user-overridden) + WIDTH (injected). RED if it skips nothing (CHAR_BIT
// doubled) AND RED if it skips everything (WIDTH lost → S_UndeclaredIdentifier).
TEST(SemanticAnalyzerCSubset, ShippedConstantUserDeclWins) {
    ScratchDir sysDir{Location::Temp, "item1-goal2"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "limits.json",
        R"({ "header": "limits.h",
             "constants": [ { "name": "CHAR_BIT", "value": 8,  "type": "i32" },
                            { "name": "WIDTH",    "value": 32, "type": "i32" } ] })",
        "int CHAR_BIT = 9;\n"
        "#include <limits.h>\n"
        "int main() { return CHAR_BIT + WIDTH; }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countSymbolsNamed(model, "CHAR_BIT"), 1u)
        << "the user's CHAR_BIT wins; the descriptor's is skipped (no double-bind)";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "WIDTH (not user-declared) must still inject + resolve";
    EXPECT_FALSE(hasCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol));
}

// MF-3: a shipped constant is `isConst` — writing to it emits S_ConstViolation
// (a macro constant is not assignable), and the InvalidTree / no-declRuleNode
// symbol does NOT crash the const-violation path.
TEST(SemanticAnalyzerCSubset, WriteToShippedConstantViolatesConst) {
    ScratchDir sysDir{Location::Temp, "item1-constviol"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "limits.json",
        R"({ "header": "limits.h",
             "constants": [ { "name": "CHAR_BIT", "value": 8, "type": "i32" } ] })",
        "#include <limits.h>\nint main() { CHAR_BIT = 5; return 0; }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u)
        << "writing to a shipped constant must fail loud (it is not assignable)";
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

// ── c26 D-CSUBSET-ABSTRACT-DECLARATOR-TYPE-NAME — fn-ptr cast/sizeof typing ──
//
// The shared `castTypeRef` now routes an abstract `directDeclarator` tail
// through `declaratorDeclaredType` (the SAME path params use), so a cast to an
// abstract fn-pointer type yields exactly `Ptr<FnSig(params)->base>`. These pins
// assert the EXACT interned shape (red-on-disable: drop the directDeclaredType
// fold and the cast mistypes as the bare base `int`).

namespace {
// Find the first castExpr node across a CU's trees (helper for the typing pins).
[[nodiscard]] inline std::pair<TreeId, NodeId>
firstCastNode(CompilationUnit const& cu) {
    for (auto const& t : cu.trees()) {
        if (!t.hasSchema()) continue;
        auto const rid = t.schema().rules().find("castExpr");
        if (!rid.valid()) continue;
        for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
            NodeId const n{i};
            if (t.kind(n) == NodeKind::Internal && t.rule(n).v == rid.v)
                return {t.id(), n};
        }
    }
    return {TreeId{}, NodeId{}};
}
} // namespace

// `(int(*)(void))p` types as `Ptr<FnSig(void)->I32>` — the exact param-position
// type. Asserts the full interned shape: outer Ptr, inner FnSig, zero params
// (the C 6.7.6.3p10 `(void)` normalization), I32 result.
TEST(SemanticAnalyzerCSubset, AbstractFnPtrCastTypesAsPtrToFnVoidInt) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { void* p; return ((int(*)(void))p) != 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    auto [tid, cast] = firstCastNode(*cu);
    ASSERT_TRUE(cast.valid());
    TypeId const castTy = model.typeAt(cast);
    ASSERT_TRUE(castTy.valid()) << "the fn-ptr cast node must be typed";
    ASSERT_EQ(ti.kind(castTy), TypeKind::Ptr) << "cast result is a pointer";
    TypeId const pointee = ti.operands(castTy)[0];
    ASSERT_EQ(ti.kind(pointee), TypeKind::FnSig)
        << "the pointee must be a function signature (Ptr<Fn ...>)";
    EXPECT_EQ(ti.fnParams(pointee).size(), 0u)
        << "(void) normalizes to zero params";
    EXPECT_EQ(ti.kind(ti.fnResult(pointee)), TypeKind::I32)
        << "the fn returns int";
    EXPECT_FALSE(ti.fnIsVariadic(pointee));
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
}

// `(int(*)(int))p` — a one-param fn-ptr type: `Ptr<FnSig(int)->I32>`.
TEST(SemanticAnalyzerCSubset, AbstractFnPtrCastWithParamTypesCorrectly) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { void* p; return ((int(*)(int))p) != 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    auto [tid, cast] = firstCastNode(*cu);
    ASSERT_TRUE(cast.valid());
    TypeId const castTy = model.typeAt(cast);
    ASSERT_EQ(ti.kind(castTy), TypeKind::Ptr);
    TypeId const fn = ti.operands(castTy)[0];
    ASSERT_EQ(ti.kind(fn), TypeKind::FnSig);
    auto params = ti.fnParams(fn);
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(ti.kind(params[0]), TypeKind::I32);
    EXPECT_EQ(ti.kind(ti.fnResult(fn)), TypeKind::I32);
}

// sizeof of an abstract fn-ptr type stamps size_t and resolves the type (no
// crash) — the Pass-2 form. (The array-dimension FOLD readback is pinned in the
// corpus runtime example, which exercises the value end-to-end.)
TEST(SemanticAnalyzerCSubset, SizeofAbstractFnPtrResolvesCleanly) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { return (int)sizeof(int(*)(void)); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
}

// FAIL-LOUD: a NAMED declarator in a type-name position (`(int x)p`) is a C
// constraint violation (type-names are abstract) — S_TypeNameDeclaratorNotAbstract,
// never silently parsed as `(int)`. This is the inverse of
// S_DeclarationDeclaresNothing.
TEST(SemanticAnalyzerCSubset, NamedCastDeclaratorFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "int main() { int p; return (int (x))p; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeNameDeclaratorNotAbstract), 1u)
        << "a named type-name declarator must fail loud, never silently "
           "drop the name and cast to the bare base type";
}

// FAIL-LOUD: an UNKNOWN base type with an abstract fn-ptr declarator
// (`(Nope(*)(void))p`) still emits S_UnknownType (the base resolves to nothing —
// the declarator fold never masks a missing base).
TEST(SemanticAnalyzerCSubset, AbstractFnPtrCastUnknownBaseStillUnknownType) {
    auto model = analyzeShipped("c-subset", {
        "int main() { void* p; return ((Nope(*)(void))p) != 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownType), 1u)
        << "an unknown base type must still fail loud under an abstract "
           "fn-ptr declarator";
}

// ── c29 D-CSUBSET-POST-STAR-CAST-QUALIFIER — post-star cast qualifier stripped ──
//
// `castTypeRef`'s stars are now `pointerLayer` children; a POST-star qualifier
// (`int * const` / `u32 * volatile`) rides inside the layer. A cast yields an
// RVALUE with NO top-level cv (C 6.5.4), so the resolver STRIPS the layer's
// ptrQualifiers: `(int * const)p` and `(int *)p` intern the SAME Ptr<int>, and a
// post-star volatile builds Ptr<u32> with NO VolatileQual on the POINTER. The c27
// PRE-stars volatile pointee path (`volatile u32 *`→Ptr<VolatileQual(u32)>) is
// SEPARATE and unbroken. Red-on-disable: revert `{repeat pointerLayer}` to
// `{repeat StarOp}` → the post-star const fails to parse (P0009); keep the layer
// but fold its volatile into the base → the pointer wrongly carries VolatileQual.

namespace {
// The k-th (0-based) castExpr node across a CU's trees, source order.
[[nodiscard]] inline std::pair<TreeId, NodeId>
nthCastNode(CompilationUnit const& cu, std::size_t k) {
    std::size_t seen = 0;
    for (auto const& t : cu.trees()) {
        if (!t.hasSchema()) continue;
        auto const rid = t.schema().rules().find("castExpr");
        if (!rid.valid()) continue;
        for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
            NodeId const n{i};
            if (t.kind(n) == NodeKind::Internal && t.rule(n).v == rid.v) {
                if (seen++ == k) return {t.id(), n};
            }
        }
    }
    return {TreeId{}, NodeId{}};
}
} // namespace

// `(int * const)p` and `(int *)p` resolve to the EXACT SAME TypeId — the post-star
// const is stripped (a cast pointer is a top-level-cv-less rvalue). Both casts in
// one CU so the interned ids are directly comparable.
TEST(SemanticAnalyzerCSubset, PostStarConstCastStripsToPlainPointer) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { int* p;\n"
        "  int* a = (int * const)p;\n"
        "  int* b = (int *)p;\n"
        "  return (a == b); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    auto [tA, cA] = nthCastNode(*cu, 0);
    auto [tB, cB] = nthCastNode(*cu, 1);
    ASSERT_TRUE(cA.valid() && cB.valid());
    TypeId const tyConst = model.typeAt(cA);
    TypeId const tyPlain = model.typeAt(cB);
    ASSERT_TRUE(tyConst.valid() && tyPlain.valid());
    EXPECT_EQ(tyConst, tyPlain)
        << "(int * const)p and (int *)p must intern the SAME type (post-star "
           "const stripped)";
    ASSERT_EQ(ti.kind(tyConst), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(ti.operands(tyConst)[0]), TypeKind::I32);
    EXPECT_FALSE(ti.isVolatileQualified(tyConst))
        << "a const cast pointer is not volatile";
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
}

// `(u32 * volatile)p` — a POST-star volatile is STRIPPED: the cast types as a
// plain Ptr<u32>, with NO top-level VolatileQual on the POINTER (a cast rvalue has
// no top-level cv, C 6.5.4). The pointee is the bare u32 (NOT volatile — the
// volatile was the pointer object's, dropped).
TEST(SemanticAnalyzerCSubset, PostStarVolatileCastStripsPointerVolatile) {
    auto cu = buildShippedUnit("c-subset", {
        "typedef unsigned int u32;\n"
        "int main() { void* p; return ((u32 * volatile)p) != 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    auto [tid, cast] = nthCastNode(*cu, 0);
    ASSERT_TRUE(cast.valid());
    TypeId const castTy = model.typeAt(cast);
    ASSERT_TRUE(castTy.valid());
    ASSERT_EQ(ti.kind(castTy), TypeKind::Ptr);
    EXPECT_FALSE(ti.isVolatileQualified(castTy))
        << "the post-star volatile must be STRIPPED — no VolatileQual on the "
           "cast pointer";
    TypeId const pointee = ti.operands(castTy)[0];
    EXPECT_FALSE(ti.isVolatileQualified(pointee))
        << "an east `u32 * volatile` does NOT qualify the pointee";
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
}

// c27 UNBROKEN: `(volatile u32 *)p` — a PRE-stars volatile qualifies the POINTEE,
// building Ptr<VolatileQual(u32)>. The pointer itself is NOT volatile; its pointee
// IS. (The c29 strip applies ONLY to a layer's POST-star qualifier; the pre-stars
// head volatile path is untouched.)
TEST(SemanticAnalyzerCSubset, PreStarVolatileCastKeepsPointeeVolatile) {
    auto cu = buildShippedUnit("c-subset", {
        "typedef unsigned int u32;\n"
        "int main() { void* p; return ((volatile u32 *)p) != 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    auto [tid, cast] = nthCastNode(*cu, 0);
    ASSERT_TRUE(cast.valid());
    TypeId const castTy = model.typeAt(cast);
    ASSERT_TRUE(castTy.valid());
    ASSERT_EQ(ti.kind(castTy), TypeKind::Ptr);
    EXPECT_FALSE(ti.isVolatileQualified(castTy))
        << "the POINTER is not volatile (the pre-stars volatile binds the pointee)";
    TypeId const pointee = ti.operands(castTy)[0];
    EXPECT_TRUE(ti.isVolatileQualified(pointee))
        << "(volatile u32 *) builds Ptr<VolatileQual(u32)> — the pointee IS "
           "volatile (c27 unbroken)";
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
}

// c27 + c29 together: `(volatile u32 **)p` — pre-stars volatile pointee with TWO
// pointer levels → Ptr<Ptr<VolatileQual(u32)>>. (The sqlite 67392 frontier; both
// stars are now pointerLayers, neither carries a post-star qualifier.)
TEST(SemanticAnalyzerCSubset, PreStarVolatileDoublePtrCastBuildsNestedPointee) {
    auto cu = buildShippedUnit("c-subset", {
        "typedef unsigned int u32;\n"
        "int main() { void* p; return ((volatile u32 **)p) != 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    auto [tid, cast] = nthCastNode(*cu, 0);
    ASSERT_TRUE(cast.valid());
    TypeId const outer = model.typeAt(cast);
    ASSERT_TRUE(outer.valid());
    ASSERT_EQ(ti.kind(outer), TypeKind::Ptr);
    TypeId const mid = ti.operands(outer)[0];
    ASSERT_EQ(ti.kind(mid), TypeKind::Ptr) << "Ptr<Ptr<...>>";
    TypeId const inner = ti.operands(mid)[0];
    EXPECT_TRUE(ti.isVolatileQualified(inner))
        << "innermost pointee is VolatileQual(u32)";
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
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

// D-CSUBSET-CAST-ARRAY-DECAY (FC3.5 sweep-c3): the cast OPERAND
// undergoes array-to-pointer decay BEFORE the legality check (C
// 6.3.2.1p3) — `(char*)"abc"` is Ptr↔Ptr after decay and `(long)"xy"`
// is decay + the Ptr→integer round-trip. Pre-sweep BOTH fired
// S_InvalidCast (the matrix saw Array(Char) raw).
TEST(SemanticAnalyzerCSubset, CastOfStringLiteralDecaysAndIsAccepted) {
    auto model = analyzeShipped("c-subset", {
        "int main() {\n"
        "  char* p = (char*)\"abc\";\n"
        "  long bits = (long)\"xy\";\n"
        "  (void)p;\n"
        "  return (int)(bits - bits);\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidCast), 0u)
        << "(char*)\"abc\" and (long)\"xy\" must decay-then-cast, "
           "never S_InvalidCast";
}

// D-CSUBSET-CAST-VOID-DISCARD (FC3.5 sweep-c3): `(void)expr` admits
// EVERY operand type — scalar, pointer, struct VALUE (the type the
// castability matrix rejects hardest), parenthesized arithmetic — per
// C 6.5.4p2 / 6.3.2.2. Zero S_InvalidCast; nothing else errors either
// (the discard also serves the idiom's suppress-unused purpose: no
// unused-result diagnostic may be introduced that fires on it).
TEST(SemanticAnalyzerCSubset, VoidDiscardCastAcceptsAllOperandTypes) {
    auto model = analyzeShipped("c-subset", {
        "struct S { int x; };\n"
        "int main() {\n"
        "  struct S s;\n"
        "  double d = 1.5;\n"
        "  int* p = (int*)0;\n"
        "  (void)s;\n"
        "  (void)d;\n"
        "  (void)p;\n"
        "  (void)(1 + 2);\n"
        "  return 0;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidCast), 0u)
        << "(void)x must admit every operand type (C 6.5.4p2)";
}

// The DISCARD direction is the only legal void cast: a void VALUE as a
// cast operand stays rejected (C has no void→T conversion; mapCast has
// no arm). `(int)(void)x` — the inner discard types void, the outer
// cast must fire S_InvalidCast exactly once.
TEST(SemanticAnalyzerCSubset, CastFromVoidValueStaysRejected) {
    auto model = analyzeShipped("c-subset", {
        "int main() { int x = 1; return (int)(void)x; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidCast), 1u)
        << "void -> int must stay rejected; only the (void)expr DISCARD "
           "direction is legal";
}

// D-CSUBSET-COMPOUND-LITERAL-TYPEDEF (FC3.5 sweep-c3): `(MyT){...}`
// with a typedef name — the compound-literal type position now rides
// `castTypeRef` (bare-Identifier base) + the SAME commitRequiresTypeName
// binder triage castExpr uses, and the NEW `semantics.compoundLiterals`
// stamping resolves the typedef through the standard type-position
// resolver. Pre-sweep the bare identifier could not even PARSE in
// compound-literal type position (typeBaseAllowingStruct has no
// Identifier alt). Zero diagnostics: the typedef'd struct type flows
// into the literal and the enclosing declaration's assignability
// (struct P == MyP via alias interning).
TEST(SemanticAnalyzerCSubset, TypedefCompoundLiteralResolvesAndTypes) {
    auto model = analyzeShipped("c-subset", {
        "struct P { int x; int y; };\n"
        "typedef struct P MyP;\n"
        "int main() { struct P p = (MyP){40, 2}; return p.x; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u)
        << "(MyP){...} must resolve the typedef in compound-literal "
           "type position";
}

// The commit triage's case-4 arm: an UNKNOWN identifier followed by
// `{` commits as a compound literal (BlockOpen cannot continue a value
// reading) and then fails LOUD at semantic — S_UnknownType, exactly
// once, at the type position. Never a silent value reinterpretation.
TEST(SemanticAnalyzerCSubset, CompoundLiteralUnknownTypeNameFiresUnknownType) {
    auto model = analyzeShipped("c-subset", {
        "int main() { (zzz){1}; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 1u)
        << "(zzz){...} must commit via the `{` follower and fail loud "
           "at type resolution";
}

// The commit triage's case-3 arm: a KNOWN-VALUE identifier in the type
// position rolls the compound-literal reading BACK (the value reading
// is the meaning); `(a)` then parses as parenExpr and the orphan `{`
// is a LOUD PARSE error — C says `(a){...}` is invalid, and it must
// never be silently mis-parsed as either reading. Parse diagnostics
// live on the TREE (not the semantic model), so this pin reads the
// tree reporters directly.
TEST(SemanticAnalyzerCSubset, CompoundLiteralValueIdentifierIsLoudError) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { int a = 1; (a){2}; return a; }\n",
    });
    bool sawParseError = false;
    for (auto const& t : cu->trees()) {
        for (auto const& d : t.diagnostics().all()) {
            if (d.severity == DiagnosticSeverity::Error) {
                sawParseError = true;
            }
        }
    }
    EXPECT_TRUE(sawParseError)
        << "(a){...} with a VALUE identifier must fail LOUD at parse "
           "(C 6.5.2.5 requires a type name; the triage rolls back to "
           "the value reading whose orphan `{` cannot parse)";
}

// ── FC4 c1 stage 2b: declarator typing pins on the SHIPPED grammar ──────
//
// The synthetic-grammar engine pins live in test_declarator_engine.cpp;
// these mirror the canonical cases through the REAL c-subset config so a
// c-subset.lang.json role-wiring regression (not just an engine bug) is
// loud. Every pin walks the interner kinds/operands EXACTLY.

namespace {

[[nodiscard]] TypeId typeOfSymbol(SemanticModel const& m,
                                  std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i) {
        if (m.symbols()[i].name == name) return m.symbols()[i].type;
    }
    ADD_FAILURE() << "no symbol named '" << name << "'";
    return InvalidType;
}

// Assert `t` is exactly Ptr<FnSig([I32] -> I32)>.
void expectPtrToIntIntFnSig(TypeInterner const& in, TypeId t) {
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(in.kind(t), TypeKind::Ptr);
    ASSERT_EQ(in.operands(t).size(), 1u);
    TypeId const fn = in.operands(t)[0];
    ASSERT_EQ(in.kind(fn), TypeKind::FnSig);
    auto const params = in.fnParams(fn);
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(in.kind(params[0]), TypeKind::I32);
    EXPECT_EQ(in.kind(in.fnResult(fn)), TypeKind::I32);
}

} // namespace

// `int (*fp)(int);` — POINTER TO FUNCTION at file scope: the canonical
// C 6.7.6 inversion through the shipped declarator roles.
TEST(SemanticAnalyzerCSubset, FnPtrDeclaratorTypedAsPtrFnSig) {
    auto model = analyzeShipped("c-subset", { "int (*fp)(int);\n" });
    EXPECT_FALSE(model.hasErrors());
    expectPtrToIntIntFnSig(model.lattice().interner(),
                           typeOfSymbol(model, "fp"));
}

// `typedef int (*H)(int); H h;` — the typedef'd fn-ptr alias declares
// the SAME interned TypeId as the direct declarator form (structural
// canonicalization is the witness that the alias resolved through the
// identical inversion, not a lookalike).
TEST(SemanticAnalyzerCSubset, TypedefFnPtrAliasDeclaresSameInternedType) {
    auto model = analyzeShipped("c-subset", {
        "int (*fp)(int);\n"
        "typedef int (*H)(int);\n"
        "H h;\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const& in = model.lattice().interner();
    TypeId const th = typeOfSymbol(model, "h");
    expectPtrToIntIntFnSig(in, th);
    EXPECT_EQ(th.v, typeOfSymbol(model, "fp").v)
        << "alias-declared and directly-declared fn-ptr types must intern "
           "to ONE TypeId";
}

// `int *p, q;` — the star binds PER-DECLARATOR (C 6.7.6) on the shipped
// grammar: p : Ptr<I32>, q : I32.
TEST(SemanticAnalyzerCSubset, StarBindsPerDeclaratorOnShippedGrammar) {
    auto model = analyzeShipped("c-subset", { "int *p, q;\n" });
    EXPECT_FALSE(model.hasErrors());
    auto const& in = model.lattice().interner();
    TypeId const tp = typeOfSymbol(model, "p");
    ASSERT_TRUE(tp.valid());
    ASSERT_EQ(in.kind(tp), TypeKind::Ptr);
    EXPECT_EQ(in.kind(in.operands(tp)[0]), TypeKind::I32);
    TypeId const tq = typeOfSymbol(model, "q");
    ASSERT_TRUE(tq.valid());
    EXPECT_EQ(in.kind(tq), TypeKind::I32);
}

// `int (*arr[2])(int);` — ARRAY OF POINTER-TO-FUNCTION:
// Array<2, Ptr<FnSig([I32] -> I32)>> (suffix folds before the descent,
// stars inside the group bind first).
TEST(SemanticAnalyzerCSubset, ArrayOfFnPtrDeclaratorTyped) {
    auto model = analyzeShipped("c-subset", { "int (*arr[2])(int);\n" });
    EXPECT_FALSE(model.hasErrors());
    auto const& in = model.lattice().interner();
    TypeId const t = typeOfSymbol(model, "arr");
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(in.kind(t), TypeKind::Array);
    ASSERT_EQ(in.scalars(t).size(), 1u);
    EXPECT_EQ(in.scalars(t)[0], 2);
    expectPtrToIntIntFnSig(in, in.operands(t)[0]);
}

// C 6.7.6.3p10: `int f(void)` declares ZERO parameters — the FnSig's
// param span is EMPTY (not a one-void-param signature).
TEST(SemanticAnalyzerCSubset, VoidParamListDeclaresZeroParams) {
    auto model = analyzeShipped("c-subset", {
        "int f(void) { return 1; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const& in = model.lattice().interner();
    TypeId const tf = typeOfSymbol(model, "f");
    ASSERT_TRUE(tf.valid());
    ASSERT_EQ(in.kind(tf), TypeKind::FnSig);
    EXPECT_EQ(in.fnParams(tf).size(), 0u);
    EXPECT_EQ(in.kind(in.fnResult(tf)), TypeKind::I32);
}

// A NAMED void param is ill-formed (C 6.7.6.3p10 admits only the sole
// UNNAMED `(void)`): S_InvalidVoidParam, positioned ON the param node.
TEST(SemanticAnalyzerCSubset, NamedVoidParamFiresInvalidVoidParamPositioned) {
    auto cu = buildShippedUnit("c-subset", {
        "int g(void x) { return 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidVoidParam), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_InvalidVoidParam) continue;
        EXPECT_EQ(cu->trees()[0].source().slice(d.span), "void x")
            << "the diagnostic must span the offending param";
    }
}

// Unnamed params (C23: a lone type name in param position is ALWAYS a
// type) still contribute their types: `int h(int, char)` -> FnSig with
// exactly [I32, Char].
TEST(SemanticAnalyzerCSubset, UnnamedParamsBuildTwoParamFnSig) {
    auto model = analyzeShipped("c-subset", {
        "int h(int, char) { return 1; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const& in = model.lattice().interner();
    TypeId const th = typeOfSymbol(model, "h");
    ASSERT_TRUE(th.valid());
    ASSERT_EQ(in.kind(th), TypeKind::FnSig);
    auto const params = in.fnParams(th);
    ASSERT_EQ(params.size(), 2u);
    EXPECT_EQ(in.kind(params[0]), TypeKind::I32);
    EXPECT_EQ(in.kind(params[1]), TypeKind::Char);
}

// ── FC4 c1 stage 2b: the decl-vs-expr triage matrix END-TO-END ──────────

// `MyP * p;` after `typedef int MyP;` — the c0 probe-order + FC2 triage
// pin: the sketch KNOWS MyP is a Type, so the identVarDecl probe COMMITS
// and p types Ptr<I32> (not the expression `MyP * p`).
TEST(SemanticAnalyzerCSubset, TypedefNameStarDeclaresPointerLocal) {
    auto model = analyzeShipped("c-subset", {
        "typedef int MyP;\n"
        "int main() { MyP * p; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UndeclaredIdentifier), 0u);
    auto const& in = model.lattice().interner();
    TypeId const tp = typeOfSymbol(model, "p");
    ASSERT_TRUE(tp.valid()) << "MyP * p; must DECLARE p";
    ASSERT_EQ(in.kind(tp), TypeKind::Ptr);
    EXPECT_EQ(in.kind(in.operands(tp)[0]), TypeKind::I32);
}

// `a * b;` where a/b are KNOWN VALUES — the triage rolls back to the
// expression statement: NO new symbol is minted (main + a + b only) and
// the program is clean.
TEST(SemanticAnalyzerCSubset, ValueStarValueStaysExpressionStatement) {
    auto model = analyzeShipped("c-subset", {
        "int main() { int a = 2; int b = 3; a * b; return a; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    // main + a + b + the 2 FC12a-core builtin TYPES (__va_list_tag + va_list) — the
    // multiplication must mint NO symbol.
    EXPECT_EQ(model.symbols().size() - 1, 5u)
        << "main + a + b + __va_list_tag + va_list — the multiplication mints none";
}

// UNKNOWN `u * v;` (no `u` anywhere, single file) — the oracle-candidate
// path: the follower-operator test rolls back (the `*` continues a value
// reading), the statement stays an EXPRESSION, and BOTH unknowns fire
// positioned S_UndeclaredIdentifier. Layout:
//   "int main() {\n  u * v;\n  return 0;\n}\n"
//    0-12 line 1; line 2 starts at 13; u at 15; v at 19.
TEST(SemanticAnalyzerCSubset, UnknownStarUnknownStaysExpressionWithUndeclared) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() {\n  u * v;\n  return 0;\n}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UndeclaredIdentifier), 2u);
    bool sawU = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_UndeclaredIdentifier) continue;
        if (d.actual == "u") {
            sawU = true;
            EXPECT_EQ(d.span.start(), 15u);
            EXPECT_EQ(d.span.end(),   16u);
        }
    }
    EXPECT_TRUE(sawU) << "`u` must carry a positioned S_UndeclaredIdentifier";
}

// ── FC4 c2: indirect calls TYPE-CHECK like direct calls ─────────────────
// D-CSUBSET-FNPTR-INDIRECT-CALL closed — the c1 walls flipped to
// positive signature checking: a Ptr<FnSig> callee (bare identifier,
// cast expression, paren/deref form) unwraps to its FnSig and runs the
// SAME result-stamp + arity + per-arg path as a direct symbol call.

// (a) bare-identifier callee typed Ptr<FnSig> — clean when the args
// match, exactly one S_ArgCountMismatch on wrong arity, S_TypeMismatch
// on a wrong arg type. (c1 predecessor: BareFnPtrIdentifierCallFires
// IndirectGate pinned the S_IndirectCallNotSupported wall.)
TEST(SemanticAnalyzerCSubset, BareFnPtrCallTypesAndChecks) {
    // Clean: fp(3) against int(*)(int).
    auto clean = analyzeShipped("c-subset", {
        "int helper(int v) { return v; }\n"
        "int main() { int (*fp)(int) = &helper; return fp(3); }\n",
    });
    EXPECT_FALSE(clean.hasErrors())
        << "a well-typed indirect call must be CLEAN (the c1 wall is "
           "retired)";

    // Arity: fp(1, 2) against int(*)(int) -> exactly 1 S_ArgCountMismatch.
    auto arity = analyzeShipped("c-subset", {
        "int helper(int v) { return v; }\n"
        "int main() { int (*fp)(int) = &helper; return fp(1, 2); }\n",
    });
    EXPECT_EQ(countCode(arity.diagnostics(),
                        DiagnosticCode::S_ArgCountMismatch), 1u)
        << "indirect calls must get the SAME arity checking as direct";
    EXPECT_EQ(countCode(arity.diagnostics(),
                        DiagnosticCode::S_NotCallable), 0u);

    // Arg type: passing a pointer where the FnSig declares int.
    auto badArg = analyzeShipped("c-subset", {
        "int helper(int v) { return v; }\n"
        "int main() {\n"
        "    int (*fp)(int) = &helper;\n"
        "    int x = 1;\n"
        "    int *p = &x;\n"
        "    return fp(p);\n"
        "}\n",
    });
    EXPECT_EQ(countCode(badArg.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "indirect calls must get the SAME per-arg checking as direct";
}

// Bare function-to-pointer DECAY (C 6.3.2.1p4): a function NAME (no `&`)
// assigned / initialized / passed where a `Ptr<FnSig>` is expected decays to
// the function's address and type-checks. This is the `fp = add` regression of
// D-SEMANTIC-ASSIGN-STMT-ASSIGNABILITY-BYPASS (commit 901fe89): the new
// assign-stmt assignability check rejected bare decay (S_TypeMismatch / S0003)
// while `fp = &add` passed. The fix is the function-to-pointer decay arm in the
// SHARED `isAssignable` chokepoint, so the assignment, initializer, and
// call-argument positions all clear at once. CRITICAL: the WHOLE existing
// fnptr corpus uses `&fn`, so NO test covered the bare form — this is that pin.
// RED-ON-DISABLE: revert the isAssignable fn-decay arm and (a)/(b)/(c) each
// report S_TypeMismatch (and the corpus example fails to BUILD).
TEST(SemanticAnalyzerCSubset, BareFunctionNameDecaysToPointerInEveryPosition) {
    // (a) bare ASSIGNMENT — the exact regression.
    auto assign = analyzeShipped("c-subset", {
        "int add(int a, int b) { return a + b; }\n"
        "int main() { int (*fp)(int, int); fp = add; return fp(40, 2); }\n",
    });
    EXPECT_EQ(countCode(assign.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "a bare function name assigned to a matching function pointer "
           "(`fp = add`) must decay to its address, not fail S_TypeMismatch";

    // (b) bare INITIALIZER (no `&`).
    auto init = analyzeShipped("c-subset", {
        "int add(int a, int b) { return a + b; }\n"
        "int main() { int (*fp)(int, int) = add; return fp(40, 2); }\n",
    });
    EXPECT_EQ(countCode(init.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "a bare function name in an initializer must decay";

    // (c) bare CALL-ARGUMENT (the callback position — `fn_fnptr_callback`).
    auto callback = analyzeShipped("c-subset", {
        "int add(int a, int b) { return a + b; }\n"
        "int apply(int (*f)(int, int), int x, int y) { return f(x, y); }\n"
        "int main() { return apply(add, 40, 2); }\n",
    });
    EXPECT_EQ(countCode(callback.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "a bare function name as a call argument must decay";

    // NEGATIVE (fail-loud preserved): an INCOMPATIBLE-signature decay must
    // STILL be rejected — the decay is pinned to the SAME interned FnSig, so a
    // different parameter list interns a distinct FnSig and stays a mismatch.
    auto mismatch = analyzeShipped("c-subset", {
        "int add(int a, int b) { return a + b; }\n"
        "int main() { int (*fp)(int) = add; return 0; }\n",
    });
    EXPECT_EQ(countCode(mismatch.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "decay does NOT relax signature compatibility — `int (*)(int) = add` "
           "(add is int(int,int)) stays a loud mismatch";
}

// (b) non-identifier callee whose STAMPED type is Ptr<FnSig> (the cast
// form `((H)fp)(3)`) — clean, plus the arity-error sibling. (c1
// predecessor: CastFnPtrCalleeFiresIndirectGate pinned the wall.)
TEST(SemanticAnalyzerCSubset, CastFnPtrCalleeTypesAndChecks) {
    auto clean = analyzeShipped("c-subset", {
        "int helper(int v) { return v; }\n"
        "typedef int (*H)(int);\n"
        "int main() { int (*fp)(int) = &helper; return ((H)fp)(3); }\n",
    });
    EXPECT_FALSE(clean.hasErrors());

    auto arity = analyzeShipped("c-subset", {
        "int helper(int v) { return v; }\n"
        "typedef int (*H)(int);\n"
        "int main() { int (*fp)(int) = &helper; return ((H)fp)(1, 2); }\n",
    });
    EXPECT_EQ(countCode(arity.diagnostics(),
                        DiagnosticCode::S_ArgCountMismatch), 1u);
    EXPECT_EQ(countCode(arity.diagnostics(),
                        DiagnosticCode::S_NotCallable), 0u);
}

// (b) non-identifier callee whose stamped type is provably NOT callable
// (`((int)x)(3)` — castExpr stamped I32) -> S_NotCallable. UNCHANGED by
// FC4 c2: the triage's other-valid-type arm.
TEST(SemanticAnalyzerCSubset, CastNonCallableCalleeFiresNotCallable) {
    auto model = analyzeShipped("c-subset", {
        "int main() { int x = 1; return ((int)x)(3); }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NotCallable), 1u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArgCountMismatch), 0u);
}

// The paren-wrapped DIRECT designator `(helper)(40)` stays CLEAN — it is
// a direct call (C 6.5.1p5: parentheses preserve the designator) and is
// RUNTIME-PROVEN end-to-end (exit-42 CLI probe, 2026-06-11). FC4 c2
// UPGRADE: the callee peel lands on `helper`'s FnSig and runs the full
// signature check — so the form is no longer silently admitted, it is
// POSITIVELY checked: `(helper)(1, 2)` now fires exactly one
// S_ArgCountMismatch (no double-emission with any other path — the
// plan-lock MUST-FIX 8 pin).
TEST(SemanticAnalyzerCSubset, ParenWrappedDirectCalleeStaysClean) {
    auto model = analyzeShipped("c-subset", {
        "int helper(int v) { return v + 2; }\n"
        "int main() { return (helper)(40); }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NotCallable), 0u);

    // FC4 c2: wrong arity through the paren-wrapped designator is now
    // CAUGHT (c1 deliberately admitted it silently) — and exactly ONCE.
    auto arity = analyzeShipped("c-subset", {
        "int helper(int v) { return v + 2; }\n"
        "int main() { return (helper)(1, 2); }\n",
    });
    EXPECT_EQ(countCode(arity.diagnostics(),
                        DiagnosticCode::S_ArgCountMismatch), 1u)
        << "exactly one emission — the peel path must not double-report "
           "with the bare-identifier/refByRule paths";
    EXPECT_EQ(countCode(arity.diagnostics(),
                        DiagnosticCode::S_NotCallable), 0u);
}

// `(*fp)(3)` / `(*helper)(40)` — the deref-designator forms. FC4 c2:
// the callee peel folds `*` on a function pointer / function designator
// (C 6.5.3.2p4 — deref is the identity for call purposes, the
// designator decays right back), lands on the designator, and runs the
// full signature check: clean when well-typed, exactly one
// S_ArgCountMismatch on wrong arity. (c1 predecessor:
// DerefFnPtrCalleeStaysSemanticallySilent pinned the conservative
// silent tier, with the LIR-tier L_IndirectCallUnsupported as the
// downstream wall — both retired by the end-to-end encoding.)
TEST(SemanticAnalyzerCSubset, DerefFnPtrCalleeTypesAndChecks) {
    auto clean = analyzeShipped("c-subset", {
        "int helper(int v) { return v; }\n"
        "int main() { int (*fp)(int) = &helper; return (*fp)(3); }\n",
    });
    EXPECT_FALSE(clean.hasErrors())
        << "(*fp)(3) is a well-typed indirect call — must be clean";

    auto arity = analyzeShipped("c-subset", {
        "int helper(int v) { return v; }\n"
        "int main() { int (*fp)(int) = &helper; return (*fp)(1, 2); }\n",
    });
    EXPECT_EQ(countCode(arity.diagnostics(),
                        DiagnosticCode::S_ArgCountMismatch), 1u)
        << "the deref peel must feed the SAME arity check";
    EXPECT_EQ(countCode(arity.diagnostics(),
                        DiagnosticCode::S_NotCallable), 0u);

    // Deref of the bare DESIGNATOR (`(*helper)(40)` — operand's own
    // type is the FnSig, no pointer involved): C idiom, stays clean.
    auto derefDesignator = analyzeShipped("c-subset", {
        "int helper(int v) { return v + 2; }\n"
        "int main() { return (*helper)(40); }\n",
    });
    EXPECT_FALSE(derefDesignator.hasErrors())
        << "deref of a function designator decays right back (C "
           "6.5.3.2p4) — must be clean";
}

// ── FC12a-core (D-FC12A-VARIADIC-CALLEE): variadic-intrinsic typing pins ──────

// `va_list ap;` resolves the injected builtin typedef; va_start/va_arg/va_end of a
// proper va_list with a BUILTIN-int type arg are clean; va_arg's node types as int.
TEST(SemanticAnalyzerCSubset, VaArgWithBuiltinIntTypesClean) {
    auto model = analyzeShipped("c-subset", {
        "int sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  int t = va_arg(ap, int);\n"
        "  va_end(ap);\n"
        "  return t;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "va_list / va_start / va_arg(ap,int) / va_end must type cleanly";
}

// `va_arg(ap, T)` with T a TYPEDEF name commits the type position as a type (the
// shared castTypeRef + commitRequiresTypeName triage) and types the node as T.
TEST(SemanticAnalyzerCSubset, VaArgWithTypedefTypeResolves) {
    auto model = analyzeShipped("c-subset", {
        "typedef int MyInt;\n"
        "int sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  MyInt t = va_arg(ap, MyInt);\n"
        "  va_end(ap);\n"
        "  return t;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "va_arg(ap, MyInt) — a typedef in the type position — must resolve";
}

// `va_arg(ap, x)` where `x` is a VALUE (not a type) must FAIL LOUD: the type
// position resolves through the same resolver casts use, which emits S_UnknownType
// for a non-type name. (The red-on-disable guard against silently treating a value
// as a type — a wrong va_arg width would be a silent garbage read.)
TEST(SemanticAnalyzerCSubset, VaArgWithValueInTypePositionFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "int sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  int x = 7;\n"
        "  int t = va_arg(ap, x);\n"   // x is a VALUE, not a type
        "  va_end(ap);\n"
        "  return t;\n"
        "}\n",
    });
    EXPECT_TRUE(model.hasErrors())
        << "va_arg(ap, x) for a VALUE x must fail loud — never treat a value as a type";
    EXPECT_GT(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u)
        << "the failure must be S_UnknownType at the type position (attributable)";
}

// `va_start(ap, n)` where `ap` is NOT a va_list (a bare int) must fail loud with a
// type mismatch — the first arg must be a va_list.
TEST(SemanticAnalyzerCSubset, VaStartWithNonVaListFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "int sum(int n, ...) {\n"
        "  int notAList;\n"
        "  va_start(notAList, n);\n"
        "  return 0;\n"
        "}\n",
    });
    EXPECT_TRUE(model.hasErrors())
        << "va_start of a non-va_list first arg must fail loud";
    EXPECT_GT(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "the failure must be S_TypeMismatch on the ap operand";
}

// ── D-CSUBSET-FN-PROTOTYPE — prototype/definition merging ──────────────────
//
// Count the SURVIVING function symbols named `name` — Function-kind records that
// are NOT absorbed protos. The merge keeps a SymbolRecord per declaration (proto
// + def), but exactly one survives the binding (`!isAbsorbedProto`); that is the
// single callable symbol. (An absorbed proto record also has its kind upgraded
// to Function, so the `!isAbsorbedProto` filter is what isolates the survivor.)
[[nodiscard]] inline std::size_t
countSurvivingFns(SemanticModel const& model, std::string_view name) {
    std::size_t n = 0;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        auto const& r = model.symbols()[i];
        if (r.name == name && r.kind == DeclarationKind::Function
            && !r.isAbsorbedProto) {
            ++n;
        }
    }
    return n;
}

// (a) A prototype followed by a compatible definition MERGES: zero diagnostics,
// and exactly one surviving Function symbol for `f` (the definition; the proto
// is absorbed). RED-ON-DISABLE: revert the Pass-1.5 proto upgrade (restore the
// S_InvalidFunctionDeclarator emission) -> hasErrors() becomes true and the
// proto stays a Variable, so countSurvivingFns drops to the lone definition only
// after a redeclaration error fires (the EXPECT_FALSE(hasErrors) flips first).
TEST(SemanticAnalyzerCSubset, FnPrototypeThenDefinitionMerges) {
    auto model = analyzeShipped("c-subset", {
        "int f(int);\n"
        "int f(int x){return x;}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a prototype + a compatible definition must merge with no diagnostics";
    EXPECT_EQ(countSurvivingFns(model, "f"), 1u)
        << "exactly one surviving Function symbol for f (the definition)";
}

// (b) Proto-idempotence: multiple compatible declarations + one definition is
// well-formed (zero diagnostics, one surviving Function). C 6.7p4 permits any
// number of compatible declarations.
TEST(SemanticAnalyzerCSubset, FnPrototypeIdempotentDeclarations) {
    auto model = analyzeShipped("c-subset", {
        "int f(int);\n"
        "int f(int);\n"
        "int f(int x){return x;}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "repeated compatible prototypes + a definition must merge cleanly";
    EXPECT_EQ(countSurvivingFns(model, "f"), 1u);
}

// (c) Definition FIRST, then a redundant compatible prototype: also a clean
// merge. The definition keeps the binding; the trailing proto is absorbed. A
// later call resolves to the definition (use-resolution reads the final scope
// binding) — witnessed by zero diagnostics on a call through `f`.
TEST(SemanticAnalyzerCSubset, FnDefinitionThenPrototypeMerges) {
    auto model = analyzeShipped("c-subset", {
        "int f(int x){return x;}\n"
        "int f(int);\n"
        "int g(void){return f(3);}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a definition followed by a redundant prototype must merge cleanly, "
           "and the call must resolve to the definition";
    EXPECT_EQ(countSurvivingFns(model, "f"), 1u);
}

// (d) Incompatible redeclaration: a prototype and a definition with DIFFERENT
// signatures (return type differs) fail loud with exactly one
// S_IncompatibleRedeclaration. RED-ON-DISABLE: make the post-1.5 sweep compare
// nothing (skip the `.v` inequality) -> the count drops to 0 and the mismatch is
// silently accepted (the definition's resolved signature would be wrong).
TEST(SemanticAnalyzerCSubset, FnPrototypeIncompatibleRedeclarationFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "int f(int);\n"
        "long f(int x){return x;}\n",
    });
    EXPECT_TRUE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 1u)
        << "an incompatible function redeclaration must fail loud exactly once";
}

// (e) A standalone prototype that is CALLED but NEVER defined is callable at the
// semantic tier (forward reference is legal — the undefined-symbol failure lands
// at HIR->MIR, see the CLI verification / corpus). The semantic phase itself
// must NOT reject the prototype: it is a valid function declaration. Zero
// diagnostics here; the call resolves to the (upgraded) Function symbol.
TEST(SemanticAnalyzerCSubset, FnPrototypeForwardCallResolvesSemantically) {
    auto model = analyzeShipped("c-subset", {
        "int f(int);\n"
        "int g(void){return f(1);}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a forward call through a prototype is legal at the semantic tier";
    EXPECT_EQ(countSurvivingFns(model, "f"), 1u)
        << "the prototype is upgraded to a callable Function symbol";
}

// ── D-CSUBSET-BLOCK-SCOPE-PROTOTYPE — a block-scope function prototype REFERS
//    to (and merges with) the file-scope function (C 6.2.2p4 / 6.7.6.3) ──
//
// (a) A block-scope prototype + a later file-scope definition MERGE: the proto
// is re-homed onto the file scope and absorbed by the definition. Zero
// diagnostics, exactly one surviving Function `f` (the file definition). A call
// inside the block resolves to it (witnessed by the corpus exit code).
// RED-ON-DISABLE: revert the Pass-1 re-home (bind in `current`) -> the block proto
// binds a separate block-local symbol that the file definition never absorbs, so
// TWO records named `f` are upgraded to Function (the block proto's kind is
// upgraded by Pass 1.5 in its own scope) -> countSurvivingFns becomes 2.
TEST(SemanticAnalyzerCSubset, BlockScopePrototypeMergesWithFileDefinition) {
    auto model = analyzeShipped("c-subset", {
        "int main(void){ int f(int); return f(2); }\n"
        "int f(int x){ return x; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a block-scope prototype + a file-scope definition must merge cleanly";
    EXPECT_EQ(countSurvivingFns(model, "f"), 1u)
        << "exactly one surviving Function symbol for f (the file definition); "
           "the block-scope proto is re-homed to file scope and absorbed";
}

// (b) Definition FIRST, then a block-scope prototype of the same function: also
// a clean merge (def keeps the binding; the block proto is absorbed). No spurious
// S_UnusedVariable from the absorbed proto (a function declaration is never an
// unused variable — the local decl's warnIfUnused is suppressed for a proto).
// RED-ON-DISABLE (the warnIfUnused suppression): drop `&& !isProto` -> the
// re-homed/absorbed block proto warns S_UnusedVariable (its own use-set is empty,
// the call resolves to the definition) -> this count becomes 1.
TEST(SemanticAnalyzerCSubset, BlockScopePrototypeAfterDefinitionNoUnusedWarning) {
    auto model = analyzeShipped("c-subset", {
        "int f(int x){ return x; }\n"
        "int main(void){ int f(int); return f(5); }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnusedVariable), 0u)
        << "an absorbed block-scope function prototype must NOT warn as an unused "
           "variable — it is a function declaration, not an object";
    EXPECT_EQ(countSurvivingFns(model, "f"), 1u);
}

// (c) Mutual recursion driven by a block-scope prototype: `even` block-declares
// `int odd(int);` and forward-calls `odd` (defined later at file scope). Both
// calls resolve; zero diagnostics; one surviving Function each.
TEST(SemanticAnalyzerCSubset, BlockScopePrototypeEnablesForwardMutualCall) {
    auto model = analyzeShipped("c-subset", {
        "int even(int n){ int odd(int); return n==0 ? 1 : odd(n-1); }\n"
        "int odd(int n){ return n==0 ? 0 : even(n-1); }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a block-scope prototype must let a function forward-call a file-scope "
           "function defined later";
    EXPECT_EQ(countSurvivingFns(model, "even"), 1u);
    EXPECT_EQ(countSurvivingFns(model, "odd"), 1u);
}

// (d) Negative (fail-loud preserved): an INCOMPATIBLE block-scope prototype and a
// file-scope definition (return type differs) fail loud with exactly one
// S_IncompatibleRedeclaration — the merge across the block→file boundary runs the
// same FnSig compatibility sweep, never silently picking a signature.
TEST(SemanticAnalyzerCSubset, BlockScopePrototypeIncompatibleWithFileDefFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "int main(void){ long f(int); return 0; }\n"
        "int f(int x){ return x; }\n",
    });
    EXPECT_TRUE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 1u)
        << "an incompatible block-scope-proto vs file-def pair must fail loud once";
}

// ── D-CSUBSET-EXTERN-DEFINITION-MERGE — an `extern` declaration MERGES with an
//    in-TU definition of the same name (the definition wins; the extern is
//    absorbed), for OBJECTS and FUNCTIONS, in both orders ──

// Count SURVIVING (non-absorbed) symbols named `name`, any kind — used for the
// extern-OBJECT merge where the survivor is a Variable, not a Function.
[[nodiscard]] inline std::size_t
countSurvivingSymbols(SemanticModel const& model, std::string_view name) {
    std::size_t n = 0;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        auto const& r = model.symbols()[i];
        if (r.name == name && !r.isAbsorbedProto) ++n;
    }
    return n;
}

// (a) extern FUNCTION declaration + a later definition MERGE: zero diagnostics,
// exactly one surviving Function (the definition; the extern is absorbed).
// RED-ON-DISABLE: revert the extern merge (`nonDefiningDeclaration` / the
// mergeOrCollideRedeclaration extern arm) -> S_RedeclaredSymbol fires and the
// merge does not happen.
TEST(SemanticAnalyzerCSubset, ExternFunctionThenDefinitionMerges) {
    auto model = analyzeShipped("c-subset", {
        "extern int f(int);\n"
        "int f(int x){ return x; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "an extern function declaration + a definition must merge cleanly";
    EXPECT_EQ(countSurvivingFns(model, "f"), 1u);
}

// (b) Definition FIRST, then a redundant `extern` function declaration: also a
// clean merge (the definition keeps the binding; the extern is absorbed).
TEST(SemanticAnalyzerCSubset, ExternFunctionAfterDefinitionMerges) {
    auto model = analyzeShipped("c-subset", {
        "int f(int x){ return x; }\n"
        "extern int f(int);\n"
        "int g(void){ return f(3); }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a definition followed by a redundant extern declaration must merge";
    EXPECT_EQ(countSurvivingFns(model, "f"), 1u);
}

// (c) extern OBJECT declaration + a definition (with initializer) MERGE: zero
// diagnostics, exactly one surviving symbol named `g` (the definition; the extern
// is absorbed). Pre-fix this collided S_RedeclaredSymbol.
// RED-ON-DISABLE: revert the extern merge -> S_RedeclaredSymbol count is 1 and two
// records named `g` survive.
TEST(SemanticAnalyzerCSubset, ExternObjectThenDefinitionMerges) {
    auto model = analyzeShipped("c-subset", {
        "extern int g;\n"
        "int g = 5;\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "an extern object declaration + a definition must merge cleanly";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 0u);
    EXPECT_EQ(countSurvivingSymbols(model, "g"), 1u)
        << "exactly one surviving symbol for g (the definition); extern absorbed";
}

// (d) Definition FIRST, then a redundant `extern` object declaration: clean merge.
TEST(SemanticAnalyzerCSubset, ExternObjectAfterDefinitionMerges) {
    auto model = analyzeShipped("c-subset", {
        "int g = 6;\n"
        "extern int g;\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a definition followed by a redundant extern declaration must merge";
    EXPECT_EQ(countSurvivingSymbols(model, "g"), 1u);
}

// (e) extern idempotence: multiple extern declarations + one definition is well-
// formed (zero diagnostics, one surviving symbol).
TEST(SemanticAnalyzerCSubset, ExternObjectIdempotentThenDefinition) {
    auto model = analyzeShipped("c-subset", {
        "extern int g;\n"
        "extern int g;\n"
        "int g = 7;\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "repeated extern declarations + a definition must merge cleanly";
    EXPECT_EQ(countSurvivingSymbols(model, "g"), 1u);
}

// (f) Negative (fail-loud preserved): an extern declaration and an INCOMPATIBLE
// definition (int vs long) fail loud with exactly one S_IncompatibleRedeclaration
// — the merge runs the same type-compat sweep, never silently picking a type.
// RED-ON-DISABLE: disable the compat sweep -> the mismatch is silently accepted.
TEST(SemanticAnalyzerCSubset, ExternObjectIncompatibleDefinitionFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "extern int g;\n"
        "long g = 5;\n",
    });
    EXPECT_TRUE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 1u)
        << "an incompatible extern + definition must fail loud exactly once";
}

// (g) Negative (fail-loud preserved): TWO real (INITIALIZED) object definitions
// still collide S_RedeclaredSymbol — the merge admits a NON-DEFINING declaration
// (extern / proto / file-scope tentative) + at most one real definition, never two
// real definitions. (c33: `int g; int g = 5;` does NOT collide — the tentative is
// non-defining; only BOTH-initialized collides.)
TEST(SemanticAnalyzerCSubset, TwoObjectDefinitionsStillCollide) {
    auto model = analyzeShipped("c-subset", {
        "int g = 1;\n"
        "int g = 2;\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "two object DEFINITIONS must still collide — only a non-defining "
           "declaration + at most one definition merge";
}

// (h) Negative (fail-loud preserved): an extern FUNCTION and a same-named OBJECT
// are different categories and must NOT merge — a genuine S_RedeclaredSymbol.
TEST(SemanticAnalyzerCSubset, ExternFunctionVsObjectCrossCategoryCollides) {
    auto model = analyzeShipped("c-subset", {
        "extern int f(int);\n"
        "int f;\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "a function and an object of the same name are different categories — "
           "they must collide, not merge";
}

// (i) Negative (fail-loud REGRESSION GUARD): a TYPEDEF (kind Type) and a same-named
// extern OBJECT (kind Variable) are DIFFERENT declaration categories and must NOT
// merge — a genuine S_RedeclaredSymbol (C 6.7p4: a typedef and an object of the same
// name in one scope conflict). The merge-or-collide guard splits on the PRECISE
// DeclarationKind; a coarse function-vs-non-function split would lump Type and
// Variable together and silently absorb the extern into the typedef.
// RED-ON-DISABLE: replace the precise `category()` with the coarse
// `priorIsFnCategory == newIsFnCategory` (both Type and Variable are "non-function"
// → sameCategory, extern non-defining → MERGE) and this count falls to 0 — the
// typedef+extern pair is silently accepted.
TEST(SemanticAnalyzerCSubset, TypedefVsExternObjectCrossCategoryCollides) {
    auto model = analyzeShipped("c-subset", {
        "typedef int g;\n"
        "extern int g;\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "a typedef (Type) and a same-named extern object (Variable) are different "
           "categories — they must collide, not silently merge";
}

// (j) Same regression guard, extern FUNCTION variant: a typedef (Type) and a same-
// named extern FUNCTION (Function) are different categories → S_RedeclaredSymbol.
// RED-ON-DISABLE: under the coarse split Type is "non-function" and the extern
// function is "function" → already differ → this variant would still collide even
// pre-fix; it guards that the PRECISE split keeps the (correct) collision rather
// than over-merging once Type stops being lumped with Variable.
TEST(SemanticAnalyzerCSubset, TypedefVsExternFunctionCrossCategoryCollides) {
    auto model = analyzeShipped("c-subset", {
        "typedef int g;\n"
        "extern int g(void);\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "a typedef (Type) and a same-named extern function (Function) are "
           "different categories — they must collide";
}

// (k) Reverse order: extern OBJECT first (Variable), then a same-named TYPEDEF
// (Type). The category guard reads BOTH records, so the collision holds regardless
// of which side is prior — symmetry pin for the precise-category fix.
TEST(SemanticAnalyzerCSubset, ExternObjectThenTypedefCrossCategoryCollides) {
    auto model = analyzeShipped("c-subset", {
        "extern int g;\n"
        "typedef int g;\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "extern object (Variable) then typedef (Type) of the same name — "
           "different categories, must collide in either order";
}

// ── c33 D-CSUBSET-TENTATIVE-DEFINITION — a file-scope object declaration WITHOUT
//    an initializer is a TENTATIVE DEFINITION (C 6.9.2): any number of tentatives
//    + at most one real (initialized) definition of the same name MERGE into one
//    object; two REAL definitions still collide. The merge reuses the
//    non-defining-declaration machinery (the tentative is folded into the
//    `mergeOrCollideRedeclaration` non-defining test) — same path as extern/proto.

// (1) Tentative definition + a later real definition MERGE: zero diagnostics,
// exactly one surviving symbol (the definition keeps the binding and its init; the
// tentative is absorbed). This is the sqlite frontier shape (`u32 t; u32 t = 0;`).
// RED-ON-DISABLE: drop `isTentativeDefinition` from the Pass-1 `newNonDef` fold ->
// the tentative is treated as a definition -> S_RedeclaredSymbol fires.
TEST(SemanticAnalyzerCSubset, TentativeDefinitionThenDefinitionMerges) {
    auto model = analyzeShipped("c-subset", {
        "int g;\n"
        "int g = 5;\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a file-scope tentative definition + a real definition must merge";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 0u);
    EXPECT_EQ(countSurvivingSymbols(model, "g"), 1u)
        << "exactly one surviving symbol for g (the definition); tentative absorbed";
}

// (2) Two tentative definitions (neither initialized) MERGE into one object (C
// 6.9.2 — it lowers to a single zero-initialized global). Zero diagnostics, one
// surviving symbol. RED-ON-DISABLE: drop the tentative fold -> S_RedeclaredSymbol.
TEST(SemanticAnalyzerCSubset, TwoTentativeDefinitionsMerge) {
    auto model = analyzeShipped("c-subset", {
        "int g;\n"
        "int g;\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "two file-scope tentative definitions must merge into one object";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 0u);
    EXPECT_EQ(countSurvivingSymbols(model, "g"), 1u);
}

// (3) `static` tentative + a `static` real definition MERGE (internal linkage does
// not change the tentative-definition rule). RED-ON-DISABLE: drop the tentative
// fold -> S_RedeclaredSymbol.
TEST(SemanticAnalyzerCSubset, StaticTentativeDefinitionThenDefinitionMerges) {
    auto model = analyzeShipped("c-subset", {
        "static int g;\n"
        "static int g = 5;\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a static tentative definition + a static definition must merge";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 0u);
    EXPECT_EQ(countSurvivingSymbols(model, "g"), 1u);
}

// (4) ★ PRESERVE — two REAL (initialized) definitions still COLLIDE
// S_RedeclaredSymbol. Both carry an initializer ⇒ both defining ⇒ not tentative.
// This is the c33 must-stay-an-error case. RED-ON-DISABLE: if the tentative gate
// stopped requiring "no initializer", an initialized def would be misread as
// tentative and this collision would vanish.
TEST(SemanticAnalyzerCSubset, TwoRealDefinitionsStillCollide_Tentative) {
    auto model = analyzeShipped("c-subset", {
        "int g = 1;\n"
        "int g = 2;\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "two REAL object definitions (both initialized) must STILL collide — "
           "the tentative merge requires an UN-initialized declaration";
}

// (5) ★ PRESERVE — a BLOCK-SCOPE duplicate is NOT a tentative definition (C 6.9.2
// is file-scope only): `int y; int y;` inside a body must STILL collide
// S_RedeclaredSymbol. RED-ON-DISABLE: if the file-scope gate were dropped, the two
// block locals would merge and this collision would vanish (a real shadowing bug).
TEST(SemanticAnalyzerCSubset, BlockScopeDuplicateNotTentativeStillCollides) {
    auto model = analyzeShipped("c-subset", {
        "int main(void){ int y; int y; return y; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "a block-scope duplicate is not a tentative definition — it must collide";
}

// (6) ★ PRESERVE — a tentative definition + an INCOMPATIBLE real definition fail
// loud with S_IncompatibleRedeclaration (NOT a silent merge). `int g;` then `g`
// redefined at an incompatible type: the merge runs the SAME post-1.5 type-compat
// sweep as extern/proto. A pointer-vs-int mismatch is target-independent (unlike
// int-vs-long, which are the SAME type under LLP64), so it conflicts on every
// target. RED-ON-DISABLE: disable the merged-decl compat sweep -> silently accepted.
TEST(SemanticAnalyzerCSubset, TentativeDefinitionIncompatibleTypeFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "int g;\n"
        "int* g = 0;\n",
    });
    EXPECT_TRUE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 1u)
        << "a tentative definition and an incompatible definition must fail loud — "
           "never a silent type merge";
}

// (7) PRESERVE (unchanged) — an `extern` declaration + a definition still merge:
// the tentative work folds ALONGSIDE the existing extern path, not over it. Guards
// that the extern arm is untouched. (Mirror of ExternObjectThenDefinitionMerges,
// re-asserted in the c33 block to lock the no-regression contract.)
TEST(SemanticAnalyzerCSubset, ExternPlusDefinitionStillMerges_TentativeGuard) {
    auto model = analyzeShipped("c-subset", {
        "extern int g;\n"
        "int g = 5;\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "an extern declaration + a definition must still merge (c33 must not "
           "regress the extern path)";
    EXPECT_EQ(countSurvivingSymbols(model, "g"), 1u);
}

// ── C 6.2.3 TAG NAMESPACE (closes the tag-namespace residue of
//    D-CSUBSET-DECL-GRAMMAR-LOW-RESIDUES) ──

// (a) `typedef struct Pair { int a; } Pair;` — the tag `Pair` (Tag namespace)
// and the typedef alias `Pair` (Ordinary namespace) share a spelling and must
// NOT collide. ZERO S_RedeclaredSymbol.
// RED-ON-DISABLE: route the composite tag BIND back through the Ordinary
// namespace (drop the `fieldChildren` → Tag gate at the bind site) and the
// alias collides with the tag → this count becomes 1.
TEST(SemanticAnalyzerCSubset, TypedefTagSameNameAsAliasNoCollision) {
    auto model = analyzeShipped("c-subset", {
        "typedef struct Pair { int a; } Pair;\n"
        "int main(void) { Pair p; p.a = 0; return p.a; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "C 6.2.3: a struct tag and a typedef alias of the same name are in "
           "SEPARATE namespaces and must not collide";
    EXPECT_FALSE(model.hasErrors());
}

// (b) Both namespaces RESOLVE: with `typedef struct Pair {…} Pair;`, a `struct
// Pair x;` (tag, via the type-position tag-ref early-arm MF-1) AND a `Pair y;`
// (alias, via the Ordinary leaf arm) both resolve to the struct type — NO
// S_UnknownType.
// RED-ON-DISABLE: remove the MF-1 tag-ref early-arm and `struct Pair x;`
// descends to the bare identifier, looked up Ordinary; it would resolve the
// typedef alias `Pair` (an Ordinary Type symbol) as the tag — masking the
// namespace split. Flip the BIND to Tag WITHOUT MF-1 and `struct Pair x;`
// misses entirely → S_UnknownType count rises.
TEST(SemanticAnalyzerCSubset, TagAndAliasBothResolveSameType) {
    auto model = analyzeShipped("c-subset", {
        "typedef struct Pair { int a; } Pair;\n"
        "int main(void) {\n"
        "  struct Pair x; x.a = 1;\n"
        "  Pair y; y.a = 2;\n"
        "  return x.a + y.a;\n"
        "}\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u)
        << "both `struct Pair` (Tag) and `Pair` (Ordinary alias) must resolve";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u)
        << "both resolve to the SAME struct type, so member access is clean";
    EXPECT_FALSE(model.hasErrors());
}

// (c) The negative is PRESERVED, with the c35-correct manifestation: a
// never-defined tag used BY VALUE (`struct Nope x;`) is an OBJECT of an
// INCOMPLETE type (c35: the opaque tag forward-mints incomplete, so the
// reference RESOLVES — it is no longer "unknown"; the error moves to the
// by-value object). Fail loud with S_IncompleteTypeObject. RED-ON-DISABLE: drop
// the c35 incomplete-object guard and `struct Nope x;` silently accepts a
// zero-size object at the semantic tier.
TEST(SemanticAnalyzerCSubset, UnknownTagByValueFiresIncompleteObject) {
    auto model = analyzeShipped("c-subset", {
        "typedef struct Pair { int a; } Pair;\n"
        "int main(void) { struct Nope x; return 0; }\n",
    });
    EXPECT_TRUE(model.hasErrors())
        << "a by-value object of a never-defined struct tag must fail loud";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompleteTypeObject), 1u)
        << "an object of an incomplete (forward-only) struct type fails loud once";
}

// A struct TAG `S` and an ordinary OBJECT `S` coexist in one scope chain and
// resolve independently (the semantic-tier mirror of the tag_ordinary_coexist
// corpus). No collision, both resolvable.
// RED-ON-DISABLE: single-namespace table → the local `int S` collides with the
// tag `S` → S_RedeclaredSymbol count becomes 1.
TEST(SemanticAnalyzerCSubset, TagAndOrdinaryObjectSameNameCoexist) {
    auto model = analyzeShipped("c-subset", {
        "struct S { int v; };\n"
        "int main(void) { struct S a; a.v = 40; int S = 2; return a.v + S; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "a struct tag `S` and an ordinary object `S` are in separate "
           "namespaces and must coexist";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// MF-2 cross-tree mechanism: the cross-tree import-injection conflict scan
// re-keys `(name, namespace)`. The load-bearing substrate is exercised
// directly at the ScopeTree level (see test_scope_tree.cpp,
// `BindingsOfCarriesNamespaceForCrossTreeReKey`): `bindingsOf` yields each
// binding's namespace and `injectBinding` re-injects into the matching
// namespace, so a header's `struct Foo` tag and an including file's
// `typedef … Foo` alias key distinctly and do NOT false-conflict. A full
// multi-tree quote-include CU is not constructible through the in-memory
// fixture (it has no on-disk include resolver), so the mechanism — not the
// driver plumbing — is what these tests pin.

// ─────────────────────────────────────────────────────────────────────────
// c23 (D-CSUBSET-STRUCT-MULTI-DECLARATOR): a struct/union member is a
// comma-separated LIST of declarators sharing ONE head base type
// (`struct S { int *a, *b; };` — C 6.7.2.1). Each slot carries its OWN
// pointer/array/fn suffix AND its OWN bitfield suffix; only the HEAD base
// type is shared. These tests pin: per-slot suffix isolation (the silent
// layout-miscompile crux), per-slot independent bitfield widths, and that
// the single-declarator form is byte-identical (regression).
// ─────────────────────────────────────────────────────────────────────────

namespace {
// The composed Struct/Union TypeId for the tag `name` (the symbol the
// fieldChildren pass interned). Returns InvalidType if not found / unresolved.
[[nodiscard]] TypeId composedAggregate(SemanticModel const& model,
                                       std::string_view name) {
    auto const& ti = model.lattice().interner();
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        SymbolRecord const& r = model.symbols()[i];
        if (r.name != name || !r.type.valid()) continue;
        TypeKind const k = ti.kind(r.type);
        if (k == TypeKind::Struct || k == TypeKind::Union) return r.type;
    }
    return {};
}
// The minted field symbol named `field` (any scope). nullptr if absent.
[[nodiscard]] SymbolRecord const* fieldSym(SemanticModel const& model,
                                           std::string_view field) {
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == field) return &model.symbols()[i];
    return nullptr;
}
} // namespace

// (a) The suffix-leak pin: `struct S { int *a, b[4], *c; };` -> three fields
// Ptr<int>, Array<int,4>, Ptr<int> at fieldIndex 0/1/2. Each declarator's
// star/array binds PER-SLOT -- only `int` (the head) is shared. RED-ON-DISABLE:
// if a slot's suffix leaked, b would be a pointer or a/c would be plain int.
TEST(SemanticAnalyzerCSubset, MultiMemberPerSlotSuffixIsolated) {
    auto model = analyzeShipped("c-subset", {
        "struct S { int *a, b[4], *c; };\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const& ti = model.lattice().interner();
    TypeId const s = composedAggregate(model, "S");
    ASSERT_TRUE(s.valid()) << "struct S must compose (all three fields resolve)";
    ASSERT_EQ(ti.operands(s).size(), 3u) << "three members in declaration order";
    // field 0: int *a  -> Ptr<int>
    EXPECT_EQ(ti.kind(ti.operands(s)[0]), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(ti.operands(ti.operands(s)[0])[0]), TypeKind::I32);
    // field 1: int b[4] -> Array<int,4>  (the `*` did NOT leak onto b)
    ASSERT_EQ(ti.kind(ti.operands(s)[1]), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ti.operands(s)[1])[0], 4);
    EXPECT_EQ(ti.kind(ti.operands(ti.operands(s)[1])[0]), TypeKind::I32);
    // field 2: int *c  -> Ptr<int>
    EXPECT_EQ(ti.kind(ti.operands(s)[2]), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(ti.operands(ti.operands(s)[2])[0]), TypeKind::I32);
    // Per-symbol fieldIndex contiguous 0..2.
    SymbolRecord const* a = fieldSym(model, "a");
    SymbolRecord const* b = fieldSym(model, "b");
    SymbolRecord const* c = fieldSym(model, "c");
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr); ASSERT_NE(c, nullptr);
    EXPECT_EQ(a->fieldIndex, 0u);
    EXPECT_EQ(b->fieldIndex, 1u);
    EXPECT_EQ(c->fieldIndex, 2u);
}

// (b) FIX 3 -- the mixed-pointer silent-miscompile pin: `struct S { int *a, b; };`
// -> a is Ptr<int>, b is I32, DISTINCTLY. sizeof==16 is NECESSARY-NOT-SUFFICIENT
// (both correct Ptr8+int4->16 AND the wrong "head-star leaks to both" Ptr8+Ptr8->16
// give 16). The load-bearing assertions are the per-field TYPES (b is I32, not a
// second pointer). RED-ON-DISABLE: a leaked head star makes b a Ptr.
TEST(SemanticAnalyzerCSubset, MultiMemberHeadStarBindsPerDeclaratorNotShared) {
    auto model = analyzeShipped("c-subset", {
        "struct S { int *a, b; };\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const& ti = model.lattice().interner();
    TypeId const s = composedAggregate(model, "S");
    ASSERT_TRUE(s.valid());
    ASSERT_EQ(ti.operands(s).size(), 2u);
    // a: Ptr<int>
    ASSERT_EQ(ti.kind(ti.operands(s)[0]), TypeKind::Ptr)
        << "the `*` binds to a";
    EXPECT_EQ(ti.kind(ti.operands(ti.operands(s)[0])[0]), TypeKind::I32);
    // b: I32 -- DISTINCTLY NOT a pointer (the crux: the head `*` must not leak).
    EXPECT_EQ(ti.kind(ti.operands(s)[1]), TypeKind::I32)
        << "b shares only the head base `int`, NOT the `*` -- a leaked star "
           "would make b a second pointer (both give sizeof 16, so the TYPE "
           "is the load-bearing assertion)";
    EXPECT_NE(ti.kind(ti.operands(s)[1]), TypeKind::Ptr);
    // Per-symbol direct type checks (independent of the composed-operand path).
    SymbolRecord const* a = fieldSym(model, "a");
    SymbolRecord const* b = fieldSym(model, "b");
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr);
    EXPECT_EQ(ti.kind(a->type), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(b->type), TypeKind::I32);
}

// (c) Per-slot bitfield widths: `struct S { int a : 3, b : 5; };` -> widths 3
// and 5 INDEPENDENTLY. The bitfield suffix is now INSIDE each member-list slot,
// so the resolve searches from the per-slot dNode. RED-ON-DISABLE: a search
// from the whole structField (the c10 root) finds the FIRST suffix for both ->
// a:3, b:3.
TEST(SemanticAnalyzerCSubset, MultiMemberPerSlotBitfieldWidths) {
    auto model = analyzeShipped("c-subset", {
        "struct S { int a : 3, b : 5; };\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const& ti = model.lattice().interner();
    TypeId const s = composedAggregate(model, "S");
    ASSERT_TRUE(s.valid());
    ASSERT_EQ(ti.operands(s).size(), 2u);
    auto w0 = ti.fieldBitWidth(s, 0);
    auto w1 = ti.fieldBitWidth(s, 1);
    ASSERT_TRUE(w0.has_value()); ASSERT_TRUE(w1.has_value());
    EXPECT_EQ(*w0, 3u);
    EXPECT_EQ(*w1, 5u) << "b's width resolves from its OWN slot, not a's";
    // Per-symbol mirror.
    SymbolRecord const* a = fieldSym(model, "a");
    SymbolRecord const* b = fieldSym(model, "b");
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr);
    ASSERT_TRUE(a->bitFieldWidth.has_value());
    ASSERT_TRUE(b->bitFieldWidth.has_value());
    EXPECT_EQ(*a->bitFieldWidth, 3u);
    EXPECT_EQ(*b->bitFieldWidth, 5u);
}

// (d) REGRESSION: the single-declarator form is byte-identical whether written
// as two statements or one comma list. `struct A { int a; int b; }` and
// `struct B { int a, b; }` must compose to the SAME field types/offsets. The
// composed interned types are content-keyed, so equal field types + equal field
// count over the same layout ==> the multi-declarator path did not perturb the
// single-declarator one.
TEST(SemanticAnalyzerCSubset, MultiMemberSingleVsCommaByteIdentical) {
    auto model = analyzeShipped("c-subset", {
        "struct A { int a; int b; };\n"
        "struct B { int a, b; };\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const& ti = model.lattice().interner();
    TypeId const a = composedAggregate(model, "A");
    TypeId const b = composedAggregate(model, "B");
    ASSERT_TRUE(a.valid()); ASSERT_TRUE(b.valid());
    ASSERT_EQ(ti.operands(a).size(), 2u);
    ASSERT_EQ(ti.operands(b).size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(ti.kind(ti.operands(a)[i]), TypeKind::I32);
        EXPECT_EQ(ti.kind(ti.operands(b)[i]), TypeKind::I32)
            << "field " << i << " must be I32 in BOTH the statement-per-field "
               "and comma-list forms (single-declarator unchanged)";
    }
    // No bitfields in either ==> both intern with empty scalar pools (the
    // 2-arg-overload-identical path).
    EXPECT_FALSE(ti.fieldBitWidth(a, 0).has_value());
    EXPECT_FALSE(ti.fieldBitWidth(b, 0).has_value());
}

// (e) The sqlite3.c:15516 frontier shape, made buildable: a multi-declarator
// pointer PAIR (`*next, *prev`) sharing one head tag + a `void *data` + a
// trailing `int count`. (sqlite's HashElem points at ITSELF -- `struct HashElem
// *next`; an inline SELF-referential struct-tag pointer is a SEPARATE pre-
// existing limitation -- the tag is bound AFTER its body's fields are
// type-resolved in the post-order walk -- pinned as fail-loud below and tracked
// by D-CSUBSET-SELF-REFERENTIAL-STRUCT. Here the pointer target is a distinct
// already-defined tag, isolating the c23 multi-declarator behavior.) Four
// fields, correct per-slot types.
TEST(SemanticAnalyzerCSubset, MultiMemberSqliteHashElemRepro) {
    auto model = analyzeShipped("c-subset", {
        "struct Elem { int k; };\n"
        "struct HashElem { struct Elem *next, *prev; void *data; int count; };\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const& ti = model.lattice().interner();
    TypeId const h = composedAggregate(model, "HashElem");
    ASSERT_TRUE(h.valid()) << "the HashElem multi-declarator struct must compose";
    ASSERT_EQ(ti.operands(h).size(), 4u);
    // next, prev: Ptr<struct Elem> (the `*` binds per-slot on BOTH).
    EXPECT_EQ(ti.kind(ti.operands(h)[0]), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(ti.operands(h)[1]), TypeKind::Ptr)
        << "prev must ALSO be a pointer -- its own `*`, not borrowed from next";
    // data: void*
    ASSERT_EQ(ti.kind(ti.operands(h)[2]), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(ti.operands(ti.operands(h)[2])[0]), TypeKind::Void);
    // count: int
    EXPECT_EQ(ti.kind(ti.operands(h)[3]), TypeKind::I32);
}

// (c24-b) D-CSUBSET-SELF-REFERENTIAL-STRUCT (CLOSED): an INLINE self-referential
// struct-tag pointer (`struct N { struct N *next; }`) now COMPILES — Pass 1
// FORWARD-MINTS the nominal TypeId before the body is walked, so the inner
// `struct N` reference resolves to that nominal TypeId, and field[0] is `Ptr<N>`
// (its pointee IS N). This flipped from the prior fail-loud pin the day the
// limitation was fixed (this cycle). RED-ON-DISABLE: revert the forward-mint and
// this fails (the field reverts to S_UnknownType).
TEST(SemanticAnalyzerCSubset, SelfReferentialStructCompiles) {
    auto model = analyzeShipped("c-subset", {
        "struct N { struct N *next; int v; };\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "an inline self-referential struct-tag pointer must compile "
           "(D-CSUBSET-SELF-REFERENTIAL-STRUCT closed)";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u);
    auto const& ti = model.lattice().interner();
    TypeId const n = composedAggregate(model, "N");
    ASSERT_TRUE(n.valid()) << "the self-referential struct must compose";
    EXPECT_FALSE(ti.isIncompleteComposite(n));     // completed
    ASSERT_EQ(ti.operands(n).size(), 2u);
    // field[0] = next: Ptr<N> whose pointee IS N (the self-reference).
    ASSERT_EQ(ti.kind(ti.operands(n)[0]), TypeKind::Ptr);
    EXPECT_EQ(ti.operands(ti.operands(n)[0])[0].v, n.v)
        << "the self-ref field's pointee must be the SAME nominal TypeId";
    // field[1] = v: int.
    EXPECT_EQ(ti.kind(ti.operands(n)[1]), TypeKind::I32);
}

// (c24-f) D-CSUBSET-SELF-REFERENTIAL-STRUCT: a DIRECT (non-pointer) self-by-value
// member is ILL-FORMED (infinite size) — fail loud with S_IncompleteTypeMember.
// The POINTER form above is the legal one; this is its fail-loud counterpart.
// RED-ON-DISABLE: drop the incomplete-member guard and this stops erroring.
TEST(SemanticAnalyzerCSubset, SelfByValueStructMemberFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "struct N { struct N n; int v; };\n",
    });
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompleteTypeMember), 1u)
        << "a struct that contains ITSELF by value has infinite size -- must fail "
           "loud (S_IncompleteTypeMember), never silently fold its size to 0";
}

// (c24-c) typedef self-reference: `typedef struct N N; struct N { N *next; };`.
// The typedef alias `N` resolves (via the tag/typedef) to the same nominal type;
// `N *next` inside the body is the self-reference through the alias.
TEST(SemanticAnalyzerCSubset, TypedefSelfReferentialStructCompiles) {
    auto model = analyzeShipped("c-subset", {
        "typedef struct N N;\n"
        "struct N { struct N *next; int v; };\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a typedef-forward-declared self-referential struct must compile";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u);
}

// (c24-d) MUTUALLY-recursive structs via the IMPLICIT incomplete-tag form:
// `struct A { struct B *b; }; struct B { struct A *a; };`. A's body references
// `struct B` by pointer BEFORE B is defined; Pass-1 forward-mints BOTH tags
// (whole-tree pre-order) so the pointer resolves to an incomplete `struct B`,
// completed when B's body is processed, and B then references A. (c35 NOTE: a
// BARE `struct B;` forward-declaration STATEMENT now ALSO works —
// D-CSUBSET-FORWARD-STRUCT-DECLARATION — but the IMPLICIT pointer form here is
// the original c24 path and is kept as its own pin. An earlier version of this
// test used a bare `struct B;` and was FALSE-GREEN: `model.hasErrors()` reads
// only the semantic reporter and was blind to the then-PARSE-error; today the
// bare form parses, but this test stays on the implicit form to pin c24.)
TEST(SemanticAnalyzerCSubset, MutuallyRecursiveStructsCompile) {
    auto model = analyzeShipped("c-subset", {
        "struct A { struct B *b; int x; };\n"
        "struct B { struct A *a; int y; };\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "mutually-recursive structs (implicit incomplete tag via pointer) must compile";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u);
}

// (e2) Multi-declarator UNION members route through the same member-list
// mechanism (`union U { int *p, n; };`). A union variant per slot; p is a
// pointer, n is int.
TEST(SemanticAnalyzerCSubset, MultiMemberUnionPerSlotSuffixIsolated) {
    auto model = analyzeShipped("c-subset", {
        "union U { int *p, n; };\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const& ti = model.lattice().interner();
    TypeId const u = composedAggregate(model, "U");
    ASSERT_TRUE(u.valid());
    ASSERT_EQ(ti.operands(u).size(), 2u);
    EXPECT_EQ(ti.kind(ti.operands(u)[0]), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(ti.operands(u)[1]), TypeKind::I32);
}

// (f) The degenerate forms still behave: an anonymous single-slot bit-field
// (`int : 5;`) is a packing slot (no named symbol, no declares-nothing), and
// `int ;` declares nothing (loud). These exercise the member-list slot with an
// ABSENT inner declarator -- the c10 anonymous/declares-nothing paths preserved.
TEST(SemanticAnalyzerCSubset, MultiMemberAnonymousBitfieldStillResolves) {
    auto model = analyzeShipped("c-subset", {
        "struct S { int a : 3; int : 5; int b; };\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeclarationDeclaresNothing), 0u);
    EXPECT_FALSE(model.hasErrors());
    SymbolRecord const* a = fieldSym(model, "a");
    SymbolRecord const* b = fieldSym(model, "b");
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr);
    ASSERT_TRUE(a->bitFieldWidth.has_value());
    EXPECT_EQ(*a->bitFieldWidth, 3u);
}

TEST(SemanticAnalyzerCSubset, MultiMemberDeclaresNothingStillLoud) {
    auto model = analyzeShipped("c-subset", {
        "struct S { int ; };\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeclarationDeclaresNothing), 1u)
        << "`int ;` declares nothing -- must stay loud (anonymous non-bitfield)";
}

// ─────────────────────────────────────────────────────────────────────────
// c25 D-CSUBSET-UNIFIED-COMPOSITE-SPECIFIER: dual-mode binder pins.
//
// ONE grammar rule (`structSpec`/`unionSpec`/`enumSpec`) is BOTH a type
// DEFINITION (body present) and a tag REFERENCE (body absent). These pins
// assert the EXACT outcome of the binder's body-child-presence routing:
// a definition MINTS the composite type and member access types through it;
// a reference RESOLVES to the prior definition; an undefined tag fails loud;
// a redefinition collides; the anonymous-typedef + nested-inline-body forms
// still type. Each is red-on-disable: break iff the dual-mode mis-routes.
// ─────────────────────────────────────────────────────────────────────────

// (3a) STRUCT define mints + reference resolves + member access types.
//   `struct S { int x; };`  — definition: mints a Struct type with one I32 field.
//   `struct S v; v.x;`      — reference: `v` resolves to the SAME Struct type,
//                             and `v.x` member access types to I32.
TEST(SemanticAnalyzerCSubset, C25StructDefineMintsRefResolvesMemberTypes) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int x; };\n"
        "int main() { struct S v; int y; y = v.x; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "define + reference + member access must be clean";
    auto const& ti = model.lattice().interner();
    // DEFINE: the tag minted a Struct type with one I32 field.
    TypeId const s = composedAggregate(model, "S");
    ASSERT_TRUE(s.valid()) << "struct S must mint a composite (definition arm)";
    ASSERT_EQ(ti.kind(s), TypeKind::Struct);
    ASSERT_EQ(ti.operands(s).size(), 1u);
    EXPECT_EQ(ti.kind(ti.operands(s)[0]), TypeKind::I32);
    // REFERENCE: `v` (declared via the body-ABSENT `struct S`) resolves to S.
    SymbolRecord const* v = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "v") v = &model.symbols()[i];
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->type.valid()) << "`struct S v;` (reference) must resolve the tag";
    EXPECT_EQ(v->type.v, s.v)
        << "the reference must resolve to the SAME TypeId the definition minted";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u)
        << "v.x where v is struct S with field x must NOT fire S_NotAComposite";
}

// (c25 SQLite-critical) FORWARD reference: a tag REFERENCED before it is DEFINED
// — the pervasive `typedef struct Foo Foo;` … `struct Foo { … };`-later idiom that
// every SQLite struct uses. The two-pass analyzer must resolve the forward
// reference to the LATER definition; the unified `structSpec` reference arm must
// preserve this EXACTLY as the former `structTypeRef` did. RED-on-disable: if the
// dual-mode routing broke forward resolution, `v.x` would fail (S_UnknownType /
// S_NotAComposite) and SQLite would regress FAR before `struct sqlite3`.
TEST(SemanticAnalyzerCSubset, C25ForwardTypedefThenDefinitionResolves) {
    auto cu = buildShippedUnit("c-subset", {
        "typedef struct Foo Foo;\n"
        "struct Foo { int x; };\n"
        "int main() { Foo v; int y; y = v.x; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "forward typedef + later definition + member access must resolve clean";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u)
        << "the forward reference must resolve to the LATER definition (two-pass)";
}

// (c25 SQLite-critical) FORWARD pointer field / mutual recursion: a struct field
// pointing at a not-yet-defined tag (`struct A { struct B *b; }; struct B {…};`).
// The field's type reference (now a body-absent `structSpec`) must resolve to the
// later `struct B` — pins that c24's self-/mutually-recursive struct support
// survives the c25 specifier unification.
TEST(SemanticAnalyzerCSubset, C25ForwardPointerFieldResolves) {
    auto cu = buildShippedUnit("c-subset", {
        "struct A { struct B *b; };\n"
        "struct B { int x; };\n"
        "int main() { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "a pointer field to a forward-declared tag must resolve to its later definition";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u);
}

// (3c) A by-VALUE object of an UNDEFINED struct fails loud. c35: the opaque tag
// forward-mints an INCOMPLETE type so `struct Nope` RESOLVES (no S_UnknownType);
// the by-value object `struct Nope v;` is then an OBJECT of incomplete type →
// S_IncompleteTypeObject. (Pre-c35 this was S_UnknownType; c35 moves the error to
// the precise constraint — an incomplete object, not an unknown type. An opaque
// `struct Nope *p` POINTER would be CLEAN.) RED-on-disable: drop the c35
// incomplete-object guard and this silently accepts a zero-size object.
TEST(SemanticAnalyzerCSubset, C35UndefinedStructTagByValueFailsLoudIncompleteObject) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { struct Nope v; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompleteTypeObject), 1u)
        << "a by-value `struct Nope v;` (undefined tag) is an incomplete object — "
           "must fail loud S_IncompleteTypeObject";
}

// (3d) Redefinition of a tag collides — S_RedeclaredSymbol, exactly as today.
TEST(SemanticAnalyzerCSubset, C25StructTagRedefinitionCollides) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int x; };\n"
        "struct S { int y; };\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "two definitions of tag S must collide exactly as before c25";
}

// (c35 forward-decl) A bare `struct S;` (no body, no object) is a FORWARD
// DECLARATION of an opaque tag (C 6.7.2.3) — it MINTS an INCOMPLETE composite
// and binds it into the Tag namespace, with NO error. (INVERTS the former
// C25BareStructForwardDeclFailsLoud, which asserted the pre-c35 S_UnknownType:
// c35 D-CSUBSET-FORWARD-STRUCT-DECLARATION deliberately changes that behavior so
// the sqlite3_stmt opaque-handle pattern compiles.) RED-on-disable: drop the
// isTagReference forward-mint and the tag misses → S_UnknownType returns and the
// `S` symbol is never an incomplete Type. The incomplete flag is the witness
// that the type stays UN-sizeable — a VALUE/by-value-member/sizeof of it fails
// loud through the unchanged computeLayout guard (covered by the dedicated
// fail-loud pins below).
TEST(SemanticAnalyzerCSubset, C35BareStructForwardDeclMintsIncompleteTag) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "`struct S;` (a forward declaration) must compile, minting an opaque tag";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u)
        << "a forward-declared tag is NOT an unknown type — it is an incomplete one";
    SymbolRecord const* s = findSym(model, "S");
    ASSERT_NE(s, nullptr) << "the forward declaration must mint a Type symbol `S`";
    EXPECT_EQ(s->kind, DeclarationKind::Type);
    ASSERT_TRUE(s->type.valid());
    EXPECT_TRUE(model.lattice().interner().isIncompleteComposite(s->type))
        << "a never-defined forward-declared `struct S` stays INCOMPLETE "
           "(un-sizeable) — the no-silent-zero-size backstop";
}

// (c35) OPAQUE handle via pointer: `typedef struct S S;` (S never defined) used
// ONLY as `S *` — the sqlite3_stmt shape. The tag-reference miss forward-mints an
// INCOMPLETE composite; the typedef alias resolves to it; `S *p` is a sizeable
// Ptr<incomplete> and the whole TU is clean. RED-on-disable: without the
// forward-mint the base `struct S` misses → S_UnknownType on both the typedef and
// the param.
TEST(SemanticAnalyzerCSubset, C35OpaqueTypedefViaPointerCompiles) {
    auto model = analyzeShipped("c-subset", {
        "typedef struct S S;\n"
        "int use(S *p){ return p ? 1 : 0; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "an opaque typedef'd struct used only by pointer must compile";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u);
    SymbolRecord const* s = findSym(model, "S");
    ASSERT_NE(s, nullptr);
    ASSERT_TRUE(s->type.valid());
    EXPECT_TRUE(model.lattice().interner().isIncompleteComposite(s->type))
        << "the opaque handle's underlying tag stays incomplete";
}

// (c35) FORWARD-then-DEFINE completes the SAME tag: `struct S; struct S { int a; };`
// — the later definition COMPLETES the forward-declared tag (no collision), and a
// member of an object of it resolves. RED-on-disable: a redefinition collision
// here (S_RedeclaredSymbol) or a member miss (S_NotAComposite) flags a broken
// forward→complete unification.
TEST(SemanticAnalyzerCSubset, C35ForwardThenDefineCompletesNoCollision) {
    auto model = analyzeShipped("c-subset", {
        "struct S;\n"
        "struct S { int a; };\n"
        "int main(void){ struct S v; v.a = 42; return v.a; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a forward declaration completed by a later definition must be clean";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "completing a forward tag is NOT a redeclaration";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NotAComposite), 0u)
        << "the member access resolves through the completed tag";
    SymbolRecord const* s = findSym(model, "S");
    ASSERT_NE(s, nullptr);
    ASSERT_TRUE(s->type.valid());
    EXPECT_FALSE(model.lattice().interner().isIncompleteComposite(s->type))
        << "after its definition the tag is COMPLETE (sizeable)";
}

// (c35 ★ fail-loud) The VALUE-of-incomplete fail-loud (`struct S v;` — a local
// OBJECT of a never-defined struct) is enforced at the STORAGE tier (the MIR
// allocaForLocal / data-producer computeLayout incomplete guard), NOT the
// semantic phase — see C35ValueOfIncompleteFailsLoud in the MIR-lowering suite
// (tests/mir/test_mir_lowering_c_subset.cpp), which runs the full pipeline.

// (c35 ★ fail-loud) MEMBER of an incomplete-pointer: `struct S; p->x` where S is
// incomplete — the member access has no layout to resolve and must FAIL LOUD
// (S_NotAComposite / the layout miss), never a wrong offset. RED-on-disable: a
// dropped incomplete guard would resolve a phantom offset.
TEST(SemanticAnalyzerCSubset, C35MemberOfIncompletePointerFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "struct S;\n"
        "int g(struct S *p){ return p->x; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_TRUE(model.hasErrors())
        << "a member access through a pointer to an incomplete struct must fail "
           "loud — its layout is unknowable";
}

// (c35 ★ fail-loud) SIZEOF of an incomplete type: `sizeof(struct S)` where S is
// incomplete is ill-formed (C 6.5.3.4) — must FAIL LOUD, never a guessed size.
// RED-on-disable: a 0 (or any) size leaking out would silently size the array.
TEST(SemanticAnalyzerCSubset, C35SizeofOfIncompleteFailsLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S;\n"
        "int a[sizeof(struct S)];\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_TRUE(model.hasErrors())
        << "sizeof of an incomplete struct must fail loud";
}

// (c35 PRESERVE) TWO DEFINITIONS still collide: `struct S { int a; }; struct S
// { int b; };` — two COMPLETE definitions of the same tag are a redefinition
// (S_RedeclaredSymbol). The forward-decl relaxation must NOT swallow this — only
// an INCOMPLETE prior tag is completable; a complete one collides. RED-on-disable:
// losing this lets a struct be silently redefined with a different layout.
TEST(SemanticAnalyzerCSubset, C35TwoDefinitionsStillCollide) {
    auto model = analyzeShipped("c-subset", {
        "struct S { int a; };\n"
        "struct S { int b; };\n",
    });
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "two complete definitions of the same tag must still collide";
}

// (3b) Anonymous typedef struct: `typedef struct { int x; } T; T v; v.x;` —
// the anonymous definition mints a Struct (via anonymousNameAllowed), the alias
// resolves, and member access types. RED-on-disable: the anonymous mint relies
// on the body child being present at the definition node.
TEST(SemanticAnalyzerCSubset, C25AnonymousTypedefStructDefinesAndResolves) {
    auto cu = buildShippedUnit("c-subset", {
        "typedef struct { int x; } T;\n"
        "int main() { T v; int y; y = v.x; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "anonymous typedef struct define + alias use + member access clean";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u);
}

// (3e) Nested inline-body field: `struct Outer { struct Inner { int x; } in; };`
// — the inner body is itself a definition (a structSpec WITH a structBody, nested
// as a field). Both compose. RED-on-disable: the recursive define path depends on
// the nested specifier being routed as a definition by its own body child.
TEST(SemanticAnalyzerCSubset, C25NestedInlineBodyFieldComposes) {
    auto cu = buildShippedUnit("c-subset", {
        "struct Outer { struct Inner { int x; } in; };\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors());
    auto const& ti = model.lattice().interner();
    TypeId const inner = composedAggregate(model, "Inner");
    TypeId const outer = composedAggregate(model, "Outer");
    ASSERT_TRUE(inner.valid()) << "nested struct Inner must compose";
    ASSERT_TRUE(outer.valid()) << "struct Outer must compose";
    ASSERT_EQ(ti.operands(outer).size(), 1u) << "Outer has one member (in)";
    EXPECT_EQ(ti.operands(outer)[0].v, inner.v)
        << "Outer's member `in` must be the inner Struct type";
}

// (3f-union) UNION define/reference parity with struct.
TEST(SemanticAnalyzerCSubset, C25UnionDefineMintsRefResolves) {
    auto cu = buildShippedUnit("c-subset", {
        "union U { int i; long l; };\n"
        "int main() { union U u; int y; y = u.i; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors());
    auto const& ti = model.lattice().interner();
    TypeId const un = composedAggregate(model, "U");
    ASSERT_TRUE(un.valid());
    EXPECT_EQ(ti.kind(un), TypeKind::Union);
    EXPECT_EQ(ti.operands(un).size(), 2u);
    SymbolRecord const* u = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "u") u = &model.symbols()[i];
    ASSERT_NE(u, nullptr);
    ASSERT_TRUE(u->type.valid());
    EXPECT_EQ(u->type.v, un.v) << "`union U u;` resolves to the minted union type";
}

// c35: the union mirror — a by-value object of an undefined union tag is an
// object of an incomplete type (the opaque tag forward-mints incomplete) →
// S_IncompleteTypeObject (pre-c35: S_UnknownType).
TEST(SemanticAnalyzerCSubset, C35UndefinedUnionTagByValueFailsLoudIncompleteObject) {
    auto model = analyzeShipped("c-subset", {
        "int main() { union Nope u; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompleteTypeObject), 1u)
        << "a by-value `union Nope u;` (undefined tag) is an incomplete object — "
           "must fail loud S_IncompleteTypeObject";
}

// (3f-enum) ENUM define/reference parity. The enum DEFINITION mints the type
// and (liftToEnclosingScope) publishes its enumerators; a bare `enum E` reference
// resolves to the same type. RED-on-disable: enumerator visibility + the
// reference-resolves leg both depend on the dual-mode routing.
TEST(SemanticAnalyzerCSubset, C25EnumDefineMintsEnumeratorsVisibleRefResolves) {
    auto cu = buildShippedUnit("c-subset", {
        "enum E { A, B, C };\n"
        "int main() { enum E e; int y; y = B; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "enum define + enumerator use (B) + bare `enum E` ref must be clean";
    // The enumerator `B` resolved (lifted to enclosing scope) — no undeclared id.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "enumerator B must resolve (liftToEnclosingScope) — no undeclared id";
    // The reference `enum E e;` resolved its tag — symbol `e` is typed.
    SymbolRecord const* e = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "e") e = &model.symbols()[i];
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->type.valid())
        << "`enum E e;` (reference) must resolve the enum tag to a type";
}

TEST(SemanticAnalyzerCSubset, C25UndefinedEnumTagFailsLoudUnknownType) {
    auto model = analyzeShipped("c-subset", {
        "int main() { enum Nope e; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 1u)
        << "a bare `enum Nope` (undefined tag) must fail loud S_UnknownType";
}

// ─────────────────────────────────────────────────────────────────────────
// c28 D-CSUBSET-LOCAL-TYPE-DEFINITION: a BLOCK-SCOPED struct/union/enum
// DEFINITION with NO declarator (`struct S { int a; };` as a STATEMENT inside
// a function — sqlite3.c:68508 walMergesort). The varDecl init-declarator-list
// became OPTIONAL (mirroring topLevelDecl), so the unified c25 structSpec
// defines the type in the ENCLOSING BLOCK scope; a later `struct S v;` resolves
// it. These pins assert the NODE SHAPE (a `varDecl` holding a `structSpec` with
// a `structBody` child and NO initDeclaratorList), the RESOLVED type of the
// defining tag + the later reference, the union/enum twins, BLOCK-SCOPING
// non-leak (the c27 lesson — a same-name outer tag stays distinct), and that a
// NON-defining no-declarator (`int;`) is NOT silently accepted at the semantic
// tier (it parses + types clean; the loud declares-nothing is HIR-tier, pinned
// in the HIR suite). Each is red-on-disable.
// ─────────────────────────────────────────────────────────────────────────

// (c28a) NODE SHAPE + RESOLVED TYPE: a local `struct S { int a; int b; };`
// parses to a `varDecl` whose head holds a defining `structSpec` (a `structBody`
// child present) and which carries NO `initDeclaratorList`; the tag mints a
// 2-field Struct; the later `struct S v;` resolves `v` to the SAME TypeId and
// `v.a` types I32 (no S_NotAComposite / S_UnknownType). RED-ON-DISABLE: revert
// the optional-list grammar tweak → the bare local `struct S { … };` is a parse
// error (P0009), so this never reaches the semantic assertions.
TEST(SemanticAnalyzerCSubset, C28LocalStructDefineNodeShapeAndType) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(void){ struct S { int a; int b; }; struct S v; int y; "
        "y = v.a; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "a block-scoped struct definition + later ref + member access must be clean";
    auto const& ti = model.lattice().interner();
    // NODE SHAPE: locate a `varDecl` that contains a `structSpec` with a
    // `structBody` descendant and has NO `initDeclaratorList` descendant.
    Tree const& tree = cu->trees()[0];
    RuleId const varDeclRule  = tree.schema().rules().find("varDecl");
    RuleId const structSpec   = tree.schema().rules().find("structSpec");
    RuleId const structBody   = tree.schema().rules().find("structBody");
    RuleId const initDeclList = tree.schema().rules().find("initDeclaratorList");
    ASSERT_TRUE(varDeclRule.valid() && structSpec.valid()
                && structBody.valid() && initDeclList.valid());
    bool foundDefiningNoDeclaratorVarDecl = false;
    walkPreOrder(tree, [&](TreeCursor const& cursor) {
        NodeId const n = cursor.current();
        if (tree.kind(n) != NodeKind::Internal || tree.rule(n).v != varDeclRule.v)
            return;
        bool hasStructSpec = false, hasStructBody = false, hasInitList = false;
        walkPreOrder(tree, n, [&](TreeCursor const& inner) {
            NodeId const m = inner.current();
            if (tree.kind(m) != NodeKind::Internal) return;
            if (tree.rule(m).v == structSpec.v)   hasStructSpec = true;
            if (tree.rule(m).v == structBody.v)   hasStructBody = true;
            if (tree.rule(m).v == initDeclList.v) hasInitList   = true;
        });
        if (hasStructSpec && hasStructBody && !hasInitList)
            foundDefiningNoDeclaratorVarDecl = true;
    });
    EXPECT_TRUE(foundDefiningNoDeclaratorVarDecl)
        << "the bare local `struct S { … };` must be a varDecl with a defining "
           "structSpec (structBody present) and NO initDeclaratorList";
    // RESOLVED TYPE: the tag minted a 2-field Struct.
    TypeId const s = composedAggregate(model, "S");
    ASSERT_TRUE(s.valid()) << "the block-scoped struct S must mint a composite";
    ASSERT_EQ(ti.kind(s), TypeKind::Struct);
    ASSERT_EQ(ti.operands(s).size(), 2u);
    EXPECT_EQ(ti.kind(ti.operands(s)[0]), TypeKind::I32);
    EXPECT_EQ(ti.kind(ti.operands(s)[1]), TypeKind::I32);
    // The later `struct S v;` resolved `v` to the SAME TypeId.
    SymbolRecord const* v = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "v") v = &model.symbols()[i];
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->type.valid());
    EXPECT_EQ(v->type.v, s.v)
        << "the in-block reference must resolve to the TypeId the local define minted";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u);
}

// (c28b) UNION + ENUM local definitions: the same no-declarator path covers all
// three composite kinds (the unified c25 specifiers). RED-ON-DISABLE: the
// optional-list tweak is shared, but the union/enum bodies exercise the
// unionSpec/enumSpec define arms in block scope.
TEST(SemanticAnalyzerCSubset, C28LocalUnionAndEnumDefine) {
    auto cuU = buildShippedUnit("c-subset", {
        "int main(void){ union U { int a; int b; }; union U v; int y; "
        "y = v.a; return 0; }\n",
    });
    assertNoBuilderErrors(*cuU);
    auto mU = analyze(cuU);
    EXPECT_FALSE(mU.hasErrors()) << "a block-scoped union definition + ref must be clean";
    TypeId const u = composedAggregate(mU, "U");
    ASSERT_TRUE(u.valid());
    EXPECT_EQ(mU.lattice().interner().kind(u), TypeKind::Union);

    auto cuE = buildShippedUnit("c-subset", {
        "int main(void){ enum E { A, B, C }; enum E e; int y; "
        "y = B; e = A; return 0; }\n",
    });
    assertNoBuilderErrors(*cuE);
    auto mE = analyze(cuE);
    EXPECT_FALSE(mE.hasErrors()) << "a block-scoped enum definition + ref must be clean";
    EXPECT_EQ(countCode(mE.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "the block-scoped enumerator B must resolve (liftToEnclosingScope)";
    SymbolRecord const* e = nullptr;
    for (std::size_t i = 1; i < mE.symbols().size(); ++i)
        if (mE.symbols()[i].name == "e") e = &mE.symbols()[i];
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->type.valid());
}

// (c28c) ★ BLOCK-SCOPING NON-LEAK (the c27 lesson, c35-updated manifestation): a
// local `struct S {int a;}` minted in an INNER block must NOT be visible to a
// SIBLING/outer scope. Post-c35 the outer `struct S w;` (after the inner block
// closed) forward-mints a FRESH INCOMPLETE `struct S` (NOT the inner COMPLETE
// one) — so `w` is an object of an INCOMPLETE type → S_IncompleteTypeObject, and
// `w.a` cannot resolve. The incompleteness IS the non-leak witness: if the inner
// COMPLETE tag had leaked, `w` would be COMPLETE, `struct S w;` would be CLEAN,
// and `w.a` would silently resolve a phantom field — the exact scope-leak
// miscompile this pins. RED-ON-DISABLE: a leak makes `w` complete → no
// S_IncompleteTypeObject and the test fails.
TEST(SemanticAnalyzerCSubset, C28LocalStructDoesNotLeakToOuterScope) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(void){ { struct S { int a; }; } struct S w; w.a = 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_TRUE(model.hasErrors())
        << "the outer `struct S w;` must fail — S is block-local to the inner {}";
    EXPECT_GT(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompleteTypeObject), 0u)
        << "the outer `struct S` is a FRESH incomplete tag (the inner COMPLETE "
           "struct S must NOT leak to the enclosing scope)";
}

// (c28d) NOMINAL distinctness (c24 decl-site identity) across scopes: an OUTER
// `struct S {int a;}` and an INNER same-name `struct S {long b; long c;}` with a
// DIFFERENT layout are DISTINCT types — the inner shadows in its block, the outer
// is unaffected. Asserts two distinct composites both compose (the inner does not
// silently alias / redefine the outer). RED-ON-DISABLE: a leak/alias would make
// one definition collide (S_RedeclaredSymbol) or share a TypeId.
TEST(SemanticAnalyzerCSubset, C28InnerStructShadowsOuterDistinctType) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int a; };\n"
        "int main(void){ struct S { long b; long c; }; struct S v; "
        "(void)v; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "an inner same-name struct must shadow (not collide with) the outer";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "a block-scoped redefinition is a SHADOW, not a redeclaration collision";
    auto const& ti = model.lattice().interner();
    // Collect every composite named S; the outer (1 I32 field) and inner (2 I64
    // fields) must BOTH exist as DISTINCT TypeIds.
    TypeId outerS{}, innerS{};
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        SymbolRecord const& r = model.symbols()[i];
        if (r.name != "S" || !r.type.valid()) continue;
        if (ti.kind(r.type) != TypeKind::Struct) continue;
        if (ti.operands(r.type).size() == 1) outerS = r.type;
        if (ti.operands(r.type).size() == 2) innerS = r.type;
    }
    ASSERT_TRUE(outerS.valid()) << "the outer 1-field struct S must compose";
    ASSERT_TRUE(innerS.valid()) << "the inner 2-field struct S must compose";
    EXPECT_NE(outerS.v, innerS.v)
        << "the inner and outer struct S are nominally DISTINCT (c24 decl-site identity)";
}

// (c28e) The NON-defining no-declarator local (`int;`) parses + types CLEAN at
// the semantic tier (the per-declarator declares-nothing arm never fires — the
// list is empty). The loud declares-nothing is HIR-tier (mirroring the top-level
// `int ;`), pinned in the HIR suite. RED-ON-DISABLE: if the semantic tier started
// rejecting it, this flips. (Pairs with HirLoweringCSubset.LocalDeclaresNothing*.)
TEST(SemanticAnalyzerCSubset, C28LocalIntSemicolonSemanticallyClean) {
    auto model = analyzeShipped("c-subset", {
        "int main(void){ int; return 0; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "`int;` is semantically clean — the declares-nothing is owned by HIR";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeclarationDeclaresNothing), 0u);
}

// (c28f) A local ABSTRACT declarator (`int *;` — list NON-empty but unnamed) is
// rejected by the SEMANTIC tier's requireNamedDeclarators arm — EXACTLY ONE
// S_DeclarationDeclaresNothing, NOT double-reported by the new HIR guard (which
// fires only for an EMPTY list). RED-ON-DISABLE: if the HIR guard fired on a
// non-empty list, the HIR suite's companion would see a second diagnostic.
TEST(SemanticAnalyzerCSubset, C28LocalAbstractDeclaratorSingleDiagnostic) {
    auto model = analyzeShipped("c-subset", {
        "int main(void){ int *; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeclarationDeclaresNothing), 1u)
        << "`int *;` (abstract declarator) is rejected ONCE at the semantic tier";
}

// (c28g) REGRESSION: making the list optional must NOT break the ordinary local
// declaration forms (declarator present). `int x;` / `int x, y;` / `static int x;`
// / `struct S { … } v;` still mint their symbols and stay clean.
TEST(SemanticAnalyzerCSubset, C28OrdinaryLocalDeclsUnaffected) {
    auto model = analyzeShipped("c-subset", {
        "int main(void){ int x; int p, q; static int s; "
        "struct S { int a; } v; x = 1; p = 2; q = 3; s = 4; v.a = 5; "
        "return x + p + q + s + v.a; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "ordinary local declarations must be unaffected by the optional list";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeclarationDeclaresNothing), 0u);
    for (char const* want : {"x", "p", "q", "s", "v"}) {
        bool found = false;
        for (std::size_t i = 1; i < model.symbols().size(); ++i)
            if (model.symbols()[i].name == want) found = true;
        EXPECT_TRUE(found) << "local symbol `" << want << "` must be minted";
    }
}

// ─────────────────────────────────────────────────────────────────────────
// c30 D-CSUBSET-LOCAL-TYPEDEF: a BLOCK-SCOPED `typedef` as a STATEMENT inside a
// function (sqlite3.c:187603 `typedef void(*LOGFUNC_t)(void*,int,const char*);`).
// `typedefDecl` is now a `statement` alternative; the alias binds into the
// enclosing BLOCK scope (Ordinary namespace) and resolves there — the whole
// typedef-name machinery (Pass-1 bind, the resolver's scope walk, the parse-time
// BinderSketch oracle) was ALREADY scope-keyed, so the only change was the one
// grammar line. These pins assert: the NODE SHAPE (a `typedefDecl` nested under
// the function-body `block`, not a top-level decl), the alias's RESOLVED type +
// a later local var, the exact sqlite fn-ptr frontier shape, ★ BLOCK-SCOPE
// NON-LEAK (the c30 silent surface — the block-local alias does NOT escape its
// block, so an OUTER use of the name resolves to S_UnknownType, IDENTICAL to a
// never-defined name), and SHADOWING of an outer same-name typedef. Each is
// red-on-disable (revert the `statement` alt → the block `typedef` is a parse
// error and these never reach their assertions).
// ─────────────────────────────────────────────────────────────────────────

// (c30a) NODE SHAPE + RESOLVED TYPE. A block-scoped `typedef int (*FN_t)(int);`
// parses to a `typedefDecl` nested under the function-body `block`, binds FN_t to
// a Ptr<Fn(int)->int>, and a later `FN_t f;` resolves `f` to that pointer type.
TEST(SemanticAnalyzerCSubset, C30LocalTypedefNodeShapeAndType) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(void){ typedef int (*FN_t)(int); FN_t f; (void)f; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "a block-scoped fn-ptr typedef + later local var must be clean";
    // NODE SHAPE: a `typedefDecl` exists as a descendant of the function body `block`.
    Tree const& tree = cu->trees()[0];
    RuleId const typedefDecl = tree.schema().rules().find("typedefDecl");
    RuleId const blockRule   = tree.schema().rules().find("block");
    ASSERT_TRUE(typedefDecl.valid() && blockRule.valid());
    bool foundBlockNestedTypedef = false;
    walkPreOrder(tree, [&](TreeCursor const& cursor){
        NodeId const n = cursor.current();
        if (tree.kind(n) != NodeKind::Internal || tree.rule(n).v != blockRule.v)
            return;
        walkPreOrder(tree, n, [&](TreeCursor const& inner){
            NodeId const m = inner.current();
            if (tree.kind(m) == NodeKind::Internal && tree.rule(m).v == typedefDecl.v)
                foundBlockNestedTypedef = true;
        });
    });
    EXPECT_TRUE(foundBlockNestedTypedef)
        << "the local `typedef` must parse to a typedefDecl nested in the function body block";
    // RESOLVED TYPE: the var `f` is Ptr<Fn(int)->int>.
    auto const& ti = model.lattice().interner();
    SymbolRecord const* f = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "f") f = &model.symbols()[i];
    ASSERT_NE(f, nullptr);
    ASSERT_TRUE(f->type.valid());
    ASSERT_EQ(ti.kind(f->type), TypeKind::Ptr)
        << "a fn-ptr typedef'd variable is a pointer";
    EXPECT_EQ(ti.kind(ti.operands(f->type)[0]), TypeKind::FnSig)
        << "the pointee is the function type int(int)";
}

// (c30b) The exact sqlite3.c:187603 frontier shape: a block-scoped fn-ptr typedef
// with a void return + (void*,int,const char*) params, then a local var of that
// type. Must be clean (no S_UnknownType for the in-block typedef-name use).
TEST(SemanticAnalyzerCSubset, C30LocalTypedefFrontierShape) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(void){ typedef void (*LOGFUNC_t)(void*, int, const char*); "
        "LOGFUNC_t xLog; (void)xLog; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "the sqlite LOGFUNC_t block-scoped fn-ptr typedef must resolve clean";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u)
        << "the in-block use `LOGFUNC_t xLog;` must find the block-local alias";
    auto const& ti = model.lattice().interner();
    SymbolRecord const* x = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "xLog") x = &model.symbols()[i];
    ASSERT_NE(x, nullptr);
    ASSERT_TRUE(x->type.valid());
    EXPECT_EQ(ti.kind(x->type), TypeKind::Ptr);
}

// (c30c) ★ BLOCK-SCOPE NON-LEAK (the c30 silent surface). A typedef declared in
// an INNER block must NOT be a type-name OUTSIDE that block. The scope-keyed type
// resolver binds the alias into the inner block's scope, so an outer use `MyT v;`
// resolves to NOTHING → S_UnknownType, EXACTLY as if MyT were never defined. (The
// follower-operator triage commits `MyT v;` as a declaration speculatively — so
// it is NOT a tree-builder error; the rejection is the scope-keyed resolver at
// the SEMANTIC tier, mirroring c28c's `struct S w;` → S_UnknownType.) The control
// (in-block use) analyzes clean; the probe (outer use) does not. RED-ON-DISABLE:
// if the alias leaked to the enclosing scope, the outer `MyT v;` would resolve `v`
// to int and S_UnknownType would VANISH — a silent block-scope leak.
TEST(SemanticAnalyzerCSubset, C30LocalTypedefDoesNotLeakToOuterScope) {
    // CONTROL: the inner-block typedef + an IN-BLOCK use analyzes clean.
    auto okModel = analyzeShipped("c-subset", {
        "int main(void){ { typedef int MyT; MyT a; (void)a; } return 0; }\n",
    });
    EXPECT_FALSE(okModel.hasErrors())
        << "the inner-block typedef + in-block use must analyze clean (control)";
    // LEAK PROBE: an OUTER use of the block-local name must fail S_UnknownType.
    auto leakModel = analyzeShipped("c-subset", {
        "int main(void){ { typedef int MyT; } MyT v; (void)v; return 0; }\n",
    });
    EXPECT_TRUE(leakModel.hasErrors())
        << "the outer `MyT v;` must fail — MyT is block-local to the inner {}";
    EXPECT_GT(countCode(leakModel.diagnostics(), DiagnosticCode::S_UnknownType), 0u)
        << "a block-scoped typedef must NOT leak — the outer use resolves to "
           "S_UnknownType, identical to a never-defined name";
}

// (c30d) SHADOWING: a block-scoped typedef shadows an outer same-name typedef. An
// outer `typedef long MyT;` (I64) and an inner (block) `typedef int MyT;` (I32):
// the in-block `MyT a;` resolves to the INNER type (I32), and is NOT an
// S_RedeclaredSymbol collision. RED-ON-DISABLE: if the block typedef didn't take
// effect (or leaked/merged into the outer scope), `a` would resolve to I64.
TEST(SemanticAnalyzerCSubset, C30InnerTypedefShadowsOuterDistinctType) {
    auto cu = buildShippedUnit("c-subset", {
        "typedef long MyT;\n"
        "int main(void){ typedef int MyT; MyT a; (void)a; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "an inner same-name typedef must shadow (not collide with) the outer";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "a block-scoped typedef redefinition is a SHADOW, not a collision";
    auto const& ti = model.lattice().interner();
    SymbolRecord const* a = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "a") a = &model.symbols()[i];
    ASSERT_NE(a, nullptr);
    ASSERT_TRUE(a->type.valid());
    EXPECT_EQ(ti.kind(a->type), TypeKind::I32)
        << "the in-block `MyT a;` resolves to the INNER typedef (int), shadowing the outer (long)";
}
