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
    // (D-FC12A-VARIADIC-CALLEE — gated on the schema declaring `vaArgRule`) + the
    // 5 intrinsic builtin FUNCTIONS (SE6 builtinFunctions, minted into the same
    // CU-wide builtins scope): c103 `__umulh` (D-CSUBSET-INTRINSIC-UMULH) + c104
    // `_InterlockedCompareExchange` (D-CSUBSET-INTRINSIC-ATOMIC-CAS) + c113
    // `_ReadWriteBarrier` (D-CSUBSET-INTRINSIC-BARRIER) + c115 `_exception_code`
    // + `_exception_info` (D-WIN64-SEH-FUNCLETS SEH intrinsics) + the 6 FC17.9(b)
    // bit-count builtins (`__builtin_{popcount,clz,ctz}{,ll}`,
    // D-CSUBSET-BITCOUNT-INTRINSICS) + the 2 FC17.5
    // predefined function-name symbols (`__func__` + `__FUNCTION__`, C99
    // 6.4.2.2 — one per configured spelling per function DEFINITION, bound into
    // main's own scope; D-CSUBSET-FUNC-PREDEFINED-IDENTIFIER) + the 56 FC17.9(b)
    // C23 <stdbit.h> `__builtin_stdc_<op>_<T>` intrinsics (14 ops × 4 widths,
    // always-injected like every other builtin; D-FULLC-STDBIT) + the 2 FC17.9(d)
    // atomic explicit-order accessors (`atomic_load_explicit` +
    // `atomic_store_explicit`, always-injected builtins; D-CSUBSET-ATOMIC).
    // FC17.9(f) (D-CSUBSET-COMPLEX): + the 4 complex builtins __builtin_complex/
    // __builtin_creal/__builtin_cimag/__builtin_conj (always-injected like the rest).
    ASSERT_EQ(model.symbols().size() - 1, 79u)
        << "main + x + __va_list_tag + va_list + __umulh + "
           "_InterlockedCompareExchange + _ReadWriteBarrier + "
           "_exception_code + _exception_info + the 6 __builtin bit-count "
           "intrinsics + the 56 __builtin_stdc_* <stdbit.h> intrinsics + "
           "atomic_load_explicit + atomic_store_explicit + the 4 __builtin_complex/"
           "creal/cimag/conj complex builtins + __func__ + __FUNCTION__";
    SymbolRecord const* xRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "x") xRec = &model.symbols()[i];
    }
    ASSERT_NE(xRec, nullptr);
    ASSERT_TRUE(xRec->type.valid()) << "the int builtin must resolve to a TypeId";
    EXPECT_EQ(model.lattice().interner().kind(xRec->type), TypeKind::I32);
}

// C99 _Complex (D-CSUBSET-COMPLEX §6.2.5): `double _Complex z;` resolves the
// `[Complex, Double]` type-specifier multiset to a Complex over F64 (via the
// `complex:true` typeSpecifiers row + the interner.complex wrap). `float _Complex`
// → Complex over F32.
TEST(SemanticAnalyzerCSubset, ComplexDeclTypedAsComplex) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { double _Complex z; float _Complex w; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* z = nullptr;
    SymbolRecord const* w = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "z") z = &model.symbols()[i];
        if (model.symbols()[i].name == "w") w = &model.symbols()[i];
    }
    ASSERT_NE(z, nullptr); ASSERT_TRUE(z->type.valid());
    EXPECT_EQ(ti.kind(z->type), TypeKind::Complex);
    EXPECT_EQ(ti.kind(ti.complexElement(z->type)), TypeKind::F64);
    ASSERT_NE(w, nullptr); ASSERT_TRUE(w->type.valid());
    EXPECT_EQ(ti.kind(w->type), TypeKind::Complex);
    EXPECT_EQ(ti.kind(ti.complexElement(w->type)), TypeKind::F32);
}

// C99 _Complex (D-CSUBSET-COMPLEX): both `_Complex int` and `_Imaginary` fail LOUD,
// via the AGNOSTIC absent-multiset discipline (no per-language token-identity branch
// in the engine). `_Complex int` — ComplexKeyword IS in the specifier vocabulary (the
// complex rows), but [Complex, Int] is an invalid multiset → S_InvalidTypeSpecifier-
// Combination (the `unsigned float` precedent). `_Imaginary` — ImaginaryKeyword sits
// in NO typeSpecifiers row (pure-imaginary types unsupported, D-CSUBSET-IMAGINARY-TYPE),
// so it is not a specifier at all → resolves as an unknown type name → S_UnknownType.
TEST(SemanticAnalyzerCSubset, ComplexIntAndImaginaryFailLoud) {
    auto ci = analyzeShipped("c-subset", {"int main() { _Complex int y; }\n"});
    EXPECT_TRUE(ci.hasErrors());
    EXPECT_GT(countCode(ci.diagnostics(),
                        DiagnosticCode::S_InvalidTypeSpecifierCombination), 0u)
        << "`_Complex int` is not a valid type-specifier multiset — must fail loud";
    auto im = analyzeShipped("c-subset", {"int main() { _Imaginary x; }\n"});
    EXPECT_TRUE(im.hasErrors());
    EXPECT_GT(countCode(im.diagnostics(), DiagnosticCode::S_UnknownType), 0u)
        << "`_Imaginary` (unsupported pure-imaginary type) must fail loud";
}

// C99 _Complex (D-CSUBSET-COMPLEX / design test #10): a complex subexpression in a
// CONSTEXPR context REFUSES to fold — loud (S_ConstexprNonConstantInitializer),
// never a silently-baked constant. Complex values have NO fold representation
// (HirLiteralValue's single double cannot hold {re, im}): a complex-constructing
// builtin is not const-evaluable, and a cast whose target is complex refuses at
// the const-eval cast-target classification (not pointer/integer/bit-precise —
// the "float / aggregate cast target" NotAConstantExpression arm). So `40.0+2.0*I`
// is ALWAYS a runtime by-address construction — the anti-fold posture the
// c99_complex example's release arm witnesses. The negative control pins that the
// SAME expressions are accepted at RUNTIME (the refusal is the constexpr gate, not
// an expression rejection).
TEST(SemanticAnalyzerCSubset, ComplexConstexprInitializerRefusesToFold) {
    // (a) a complex-constructing builtin feeding a scalar accessor: not foldable.
    auto viaBuiltin = analyzeShipped("c-subset", {
        "int main(void) { constexpr double r = "
        "__builtin_creal(__builtin_complex(40.0, 2.0)); }\n"});
    EXPECT_EQ(countCode(viaBuiltin.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 1u)
        << "a complex construction in a constexpr initializer must REFUSE the "
           "fold — a clean analysis means a complex value was silently baked";
    // (b) an explicit real->complex->real cast chain: the complex cast TARGET
    // refuses at the const-eval cast classification.
    auto viaCast = analyzeShipped("c-subset", {
        "int main(void) { constexpr double d = (double)(double _Complex)2.0; }\n"});
    EXPECT_EQ(countCode(viaCast.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 1u)
        << "a cast through a complex type in a constexpr initializer must "
           "refuse to fold";
    // Negative control: WITHOUT constexpr the same expressions are legal RUNTIME
    // constructions (the refusal above is the fold gate, not a reject of complex).
    auto runtime = analyzeShipped("c-subset", {
        "int main(void) { double r = __builtin_creal(__builtin_complex(40.0, 2.0));"
        " double d = (double)(double _Complex)2.0; return (int)(r + d); }\n"});
    EXPECT_FALSE(runtime.hasErrors())
        << "the same complex expressions must analyze clean at runtime: "
        << (runtime.diagnostics().all().empty()
                ? "" : runtime.diagnostics().all()[0].actual);
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

// SE-arrays: a non-constant length at FILE scope must fail loud rather than
// guess. The engine emits S_NonConstantArrayLength and leaves the type
// unresolved (no silent pointer decay, no assumed length). VLA C1a
// (D-CSUBSET-VLA): a BLOCK-scope `int a[n]` is now a variable-length array
// (accepted at semantic, fails loud at the MIR->LIR C1b boundary — see the
// mir/lir pins); a FILE-scope non-constant length is NOT a VLA (a VLA needs
// automatic storage) and stays S_NonConstantArrayLength.
TEST(SemanticAnalyzerCSubset, NonConstantArrayLengthEmitsDiagnostic) {
    auto cu = buildShippedUnit("c-subset", {
        "int n;\n"
        "int g[n];\n",
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

// ── C11/C23 6.4.5: wide / UTF string-literal TYPING (element core per opener) ──
namespace {
// The TypeId stamped on the first `stringLiteralExpr` rule node in `cu`'s tree
// (the whole, possibly-concatenated literal). InvalidType if none / untyped.
[[nodiscard]] TypeId firstStringLiteralType(SemanticModel const& model,
                                            CompilationUnit const& cu) {
    Tree const& tree = cu.trees()[0];
    RuleId const slit = tree.schema().rules().find("stringLiteralExpr");
    TypeId found{};
    walkPreOrder(tree, [&](TreeCursor const& c) {
        NodeId const n = c.current();
        if (tree.kind(n) == NodeKind::Internal && slit.valid()
            && tree.rule(n).v == slit.v && !found.valid()) {
            found = model.typeAt(n);
        }
    });
    return found;
}
} // namespace

// `u"AB"` → Array<U16,3>; `U"AB"` → Array<U32,3>; `u8"AB"` → Array<U8,3>.
TEST(SemanticAnalyzerCSubset, WideStringLiteralElementCorePerOpener) {
    struct Case { char const* src; TypeKind core; std::int64_t len; };
    for (auto const& tc : {Case{"void f(){ u\"AB\"; }",  TypeKind::U16, 3},
                           Case{"void f(){ U\"AB\"; }",  TypeKind::U32, 3},
                           Case{"void f(){ u8\"AB\"; }", TypeKind::U8,  3},
                           Case{"void f(){ \"AB\"; }",   TypeKind::Char, 3}}) {
        auto cu = buildShippedUnit("c-subset", { tc.src });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        ASSERT_FALSE(model.hasErrors()) << tc.src;
        auto const& ti = model.lattice().interner();
        TypeId const ty = firstStringLiteralType(model, *cu);
        ASSERT_TRUE(ty.valid()) << tc.src;
        ASSERT_EQ(ti.kind(ty), TypeKind::Array) << tc.src;
        EXPECT_EQ(ti.kind(ti.operands(ty)[0]), tc.core) << tc.src;
        EXPECT_EQ(ti.scalars(ty)[0], tc.len) << tc.src;
    }
}

// `u"€"` (source bytes E2 82 AC) → ONE U16 unit → Array<U16,2> (NOT 3 bytes + NUL).
// The semantic tier UTF-8-decodes the raw bytes for the CODE-UNIT count, the same
// shared encoder the HIR tier uses — so both agree on N.
TEST(SemanticAnalyzerCSubset, WideStringBmpMultibyteCodeUnitCount) {
    auto cu = buildShippedUnit("c-subset", { "void f(){ u\"\xe2\x82\xac\"; }" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    ASSERT_FALSE(model.hasErrors());
    auto const& ti = model.lattice().interner();
    TypeId const ty = firstStringLiteralType(model, *cu);
    ASSERT_TRUE(ty.valid());
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::U16);
    EXPECT_EQ(ti.scalars(ty)[0], 2) << "U+20AC is ONE code unit + NUL";
}

// wchar_t (`L"…"`) width is FORMAT-keyed (D-FFI-STDDEF-WCHAR-PE-WIDTH): the
// format-agnostic default (direct-API) resolves to I32 (POSIX); the PE format
// resolves to U16 (Windows UTF-16 unit). This is CONFIG-DRIVEN — the
// `elementCoreByFormat` map on the WideStringStart prefix row decides it via a
// pure `resolveElementCore` lookup, NOT a hardcoded format branch.
TEST(SemanticAnalyzerCSubset, WideCharLiteralWidthIsFormatKeyed) {
    // Default (activeFormat=nullopt) → I32.
    {
        auto cu = buildShippedUnit("c-subset", { "void f(){ L\"AB\"; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        ASSERT_FALSE(model.hasErrors());
        auto const& ti = model.lattice().interner();
        TypeId const ty = firstStringLiteralType(model, *cu);
        ASSERT_TRUE(ty.valid());
        ASSERT_EQ(ti.kind(ty), TypeKind::Array);
        EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::I32)
            << "wchar_t defaults to the POSIX i32 width";
    }
    // PE format → U16.
    {
        auto cu = buildShippedUnit("c-subset", { "void f(){ L\"AB\"; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DataModel::Llp64, std::nullopt, std::nullopt,
                             ObjectFormatKind::Pe);
        ASSERT_FALSE(model.hasErrors());
        auto const& ti = model.lattice().interner();
        TypeId const ty = firstStringLiteralType(model, *cu);
        ASSERT_TRUE(ty.valid());
        ASSERT_EQ(ti.kind(ty), TypeKind::Array);
        EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::U16)
            << "wchar_t on PE is the u16 Windows UTF-16 code unit";
    }
}

// ── C11/C23 6.4.4.4: wide / UTF CHARACTER-constant TYPING (scalar core per prefix)
namespace {
// The TypeId stamped on the first `CharLiteral` BODY token in `cu`'s tree (a char
// constant is a SCALAR — the stamp is on the body token, unlike a string's expr
// node). InvalidType if none / left untyped (the wide type-drop for sizeof safety).
[[nodiscard]] TypeId firstCharLiteralType(SemanticModel const& model,
                                          CompilationUnit const& cu) {
    Tree const& tree = cu.trees()[0];
    SchemaTokenId const body = tree.schema().schemaTokens().find("CharLiteral");
    TypeId found{};
    bool seen = false;
    walkPreOrder(tree, [&](TreeCursor const& c) {
        NodeId const n = c.current();
        if (!seen && tree.kind(n) == NodeKind::Token && body.valid()
            && tree.tokenKind(n).v == body.v) {
            found = model.typeAt(n);
            seen  = true;
        }
    });
    return found;
}
} // namespace

// C23 6.4.4.4 — the NEW per-prefix TYPE rule: `'x'`→int (I32, UNCHANGED),
// `u'A'`→char16_t (U16), `U'A'`→char32_t (U32), `u8'A'`→char8_t (U8). Red-on-disable:
// without the wide override the prefixed forms all stay I32 (so `sizeof(u'A')`==4).
TEST(SemanticAnalyzerCSubset, WideCharLiteralScalarCorePerPrefix) {
    struct Case { char const* src; TypeKind core; };
    for (auto const& tc : {Case{"void f(){ 'x'; }",   TypeKind::I32},
                           Case{"void f(){ u'A'; }",  TypeKind::U16},
                           Case{"void f(){ U'A'; }",  TypeKind::U32},
                           Case{"void f(){ u8'A'; }", TypeKind::U8}}) {
        auto cu = buildShippedUnit("c-subset", { tc.src });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        ASSERT_FALSE(model.hasErrors()) << tc.src;
        auto const& ti = model.lattice().interner();
        TypeId const ty = firstCharLiteralType(model, *cu);
        ASSERT_TRUE(ty.valid()) << tc.src;
        EXPECT_EQ(ti.kind(ty), tc.core) << tc.src;
    }
}

// wchar_t (`L'x'`) width is FORMAT-keyed (D-FFI-STDDEF-WCHAR-PE-WIDTH) via the SAME
// `elementCoreByFormat` axis the wide-STRING row uses — the format-agnostic default
// resolves to I32 (POSIX), PE to U16. A pure `resolveElementCore` lookup, no
// hardcoded format branch. This is the char analog of the string test above.
TEST(SemanticAnalyzerCSubset, WideCharConstantWidthIsFormatKeyed) {
    // Default (activeFormat=nullopt) → I32.
    {
        auto cu = buildShippedUnit("c-subset", { "void f(){ L'x'; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        ASSERT_FALSE(model.hasErrors());
        auto const& ti = model.lattice().interner();
        TypeId const ty = firstCharLiteralType(model, *cu);
        ASSERT_TRUE(ty.valid());
        EXPECT_EQ(ti.kind(ty), TypeKind::I32) << "wchar_t defaults to the POSIX i32 width";
    }
    // PE format → U16.
    {
        auto cu = buildShippedUnit("c-subset", { "void f(){ L'x'; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DataModel::Llp64, std::nullopt, std::nullopt,
                             ObjectFormatKind::Pe);
        ASSERT_FALSE(model.hasErrors());
        auto const& ti = model.lattice().interner();
        TypeId const ty = firstCharLiteralType(model, *cu);
        ASSERT_TRUE(ty.valid());
        EXPECT_EQ(ti.kind(ty), TypeKind::U16)
            << "wchar_t on PE is the u16 Windows UTF-16 code unit";
    }
}

// The sizeof-safety pin (MUST-FIX #3a): a wide char whose code point does NOT fit
// its element (`u8'β'`>U+007F, `u'😀'` astral) leaves the body token UNTYPED so a
// `sizeof`/`_Alignof` of it fails loud (never a guessed size). Here we assert the
// body token is left with no valid type (the drop) — plus the format-keyed drop:
// `L'😀'` is representable under the default I32 but NOT under the pe U16.
TEST(SemanticAnalyzerCSubset, BadWideCharConstantLeavesBodyTokenUntyped) {
    // u8'β' — U+03B2 exceeds the single-UTF-8-unit range (0x7F).
    {
        auto cu = buildShippedUnit("c-subset", { "void f(){ u8'\xce\xb2'; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        TypeId const ty = firstCharLiteralType(model, *cu);
        EXPECT_FALSE(ty.valid())
            << "an out-of-range u8 char must be left untyped so sizeof fails loud";
    }
    // L'😀' under PE (U16) → astral, unrepresentable → untyped.
    {
        auto cu = buildShippedUnit("c-subset", { "void f(){ L'\xf0\x9f\x98\x80'; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DataModel::Llp64, std::nullopt, std::nullopt,
                             ObjectFormatKind::Pe);
        TypeId const ty = firstCharLiteralType(model, *cu);
        EXPECT_FALSE(ty.valid())
            << "an astral L' char under pe (u16 wchar_t) must be left untyped";
    }
    // L'😀' under the default format (I32 wchar_t) → representable → typed I32.
    {
        auto cu = buildShippedUnit("c-subset", { "void f(){ L'\xf0\x9f\x98\x80'; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        ASSERT_FALSE(model.hasErrors());
        auto const& ti = model.lattice().interner();
        TypeId const ty = firstCharLiteralType(model, *cu);
        ASSERT_TRUE(ty.valid())
            << "an astral L' char under the i32 default wchar_t IS representable";
        EXPECT_EQ(ti.kind(ty), TypeKind::I32);
    }
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
// (a) An invalid assignment STATEMENT `p = q;` (int* <- char*, distinct typed
// pointers) fails loud with a positioned S_TypeMismatch — the SAME diagnostic the
// init site `int* p = q;` emits. `q` is a parameter so no initializer adds a
// second mismatch; exactly ONE fires.
// (NOTE: this test originally used `int x; x = f;` [int <- float], but
// D-CSUBSET-INT-FLOAT-CONVERSION made int<->float an ADMITTED implicit assignment
// conversion in c-subset, so that pair is no longer a mismatch; a distinct-typed-
// pointer pair is the stable always-rejected case that still exercises the
// assignment-statement isAssignable path.)
// RED-ON-DISABLE: remove the assignment-statement isAssignable arm (restore the
// bypass) -> the assignment is silently accepted, this count drops to 0.
TEST(SemanticAnalyzerCSubset, AssignStmtIntFromIncompatiblePointerFailsLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "int sink(char* q) { int* p; p = q; return *p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "an int* <- char* assignment STATEMENT must fail loud with the same "
           "S_TypeMismatch the init (`int* p = q;`) emits — the "
           "assignment-statement assignability bypass is closed";
}

// (b) PARITY pin: the init form `int* p = q;` and the statement form `p = q;`
// must behave IDENTICALLY (both reject the same incompatible distinct-typed-
// pointer pair). Reading both in one TU yields exactly TWO S_TypeMismatch — one
// per site — proving the statement is no longer the lone unchecked position.
// (Swapped off int<-float for the same reason as (a): int<->float is now an
// admitted conversion in c-subset [D-CSUBSET-INT-FLOAT-CONVERSION].)
// RED-ON-DISABLE: with the bypass restored only the INIT fires -> count is 1.
TEST(SemanticAnalyzerCSubset, AssignStmtAndInitRejectIncompatibleIdentically) {
    auto cu = buildShippedUnit("c-subset", {
        "int sink(char* q) { int* p = q; p = q; return *p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 2u)
        << "the init site AND the assignment-statement site must each reject the "
           "int* <- char* pair — two positioned S_TypeMismatch, not one";
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
// mixed-type binary in a checked position is typed against its UNIFIED type, not
// whichever leaf the old DFS-suppressor happened to reach. The observable now uses
// a POINTER binary: D-CSUBSET-INT-SAME-SIGN-NARROW made integer narrowing implicit
// AND D-CSUBSET-INT-FLOAT-CONVERSION made int↔float implicit, so the old
// arithmetic observables (long+int→int, double+int→int) no longer fire. For
// `sink(float); int* a; int b; sink(a + b)` the argument `a + b` is `int*`
// (pointer arithmetic — `combineBinary` types `ptr + int` as the pointer) and
// `int*` is NOT assignable to the `float` param → S_TypeMismatch fires. Under the
// old suppressor it reached the `int` leaf `b`, and `int → float` IS now
// assignable, so it would be silently admitted — the exact "leaf would pass, the
// unified type fails" discrimination this closure removes. RED-ON-DISABLE: revert
// the binary arm to a leaf type and this drops to 0 (the latent unsoundness).
TEST(SemanticAnalyzerCSubset, MixedWidthBinaryArgTypedByUacNotLeaf) {
    auto cu = buildShippedUnit("c-subset", {
        "extern int sink(float v);\n"
        "int f(int* a, int b) { return sink(a + b); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // `a + b` is `int*` (pointer arith); the `float` param cannot take a pointer
    // → one mismatch. The `int` leaf `b` alone WOULD be admitted (int→float), so
    // this isolates "typed by the unified binary type, not a leaf".
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

// ── C11/C23 6.7.10 static_assert — the sizeof-folding requirement ────────────
//
// `_Static_assert(sizeof(int)==4, ...)` is the single most common idiom. The
// condition is const-evaluated by the SAME `constIntExpr` evaluator that folds
// `sizeof` in an array dimension — so it folds ONLY when analyze() is given the
// target's aggregateLayout (nullopt ⇒ deliberate fail-loud, the direct-API
// default). These pins pass AggregateLayoutParams, exactly like the array-dim
// sizeof pins above, and prove the fold is REAL (a true sizeof passes; a false
// sizeof fails loud — not a rubber-stamp).

TEST(SemanticAnalyzerCSubset, StaticAssertSizeofConditionFoldsTrue) {
    auto cu = buildShippedUnit("c-subset", {
        "_Static_assert(sizeof(int) == 4, \"int is 4\");\n"
        "int main(void){ return 42; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "sizeof(int)==4 must FOLD true in the static_assert condition";
}

TEST(SemanticAnalyzerCSubset, StaticAssertSizeofConditionFoldsFalseFailsLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "_Static_assert(sizeof(int) == 99, \"int is not 99\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 1u)
        << "sizeof(int)==99 must FOLD false — the assertion fails loud";
}

// VLA C2 (D-CSUBSET-VLA) — THE INVARIANT: `sizeof <vla>` is a RUNTIME value, NOT a
// constant expression (C 6.6). It must therefore DECLINE the const-eval fold in a
// constant-required context, so `_Static_assert(sizeof a == K)` fails loud (the
// "not an integer constant expression" branch of S_StaticAssertFailed), never folding
// to a compile-time constant. Red-on-disable for the central C2 safety property: if a
// change ever taught const-eval to fold a VLA sizeof, this assertion would either pass
// (K matched) or fail as an ordinary false assertion — either way the count/behavior
// shifts. C2 keeps the SizeOf node's `vlaArray` TypeRef so this decline holds for free.
TEST(SemanticAnalyzerCSubset, StaticAssertSizeofVlaIsNotConstantFailsLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(void){\n"
        "  volatile int s = 6;\n"
        "  int n = s;\n"
        "  int a[n];\n"                 // a VLA — sizeof a is runtime, not constant
        "  _Static_assert(sizeof a == 24, \"vla sizeof is not a constant\");\n"
        "  return 0;\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 1u)
        << "sizeof of a VLA is not a constant expression — the _Static_assert must "
           "fail loud (const-eval declines), never fold to a compile-time value";
}

// sizeof-of-a-STRUCT in the condition folds (exercises the aggregateLayout path,
// not just the scalar width). `struct S{int a; int b;}` = 8 bytes under natural
// alignment → the assertion passes; the wrong size fails loud.
TEST(SemanticAnalyzerCSubset, StaticAssertSizeofStructConditionFolds) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int a; int b; };\n"
        "_Static_assert(sizeof(struct S) == 8, \"S is 8\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "sizeof(struct S)==8 must fold through the aggregateLayout engine";
}

// The C23 1-ARG form with a sizeof condition (message-less) still folds — pins
// that the peel/parse of the 1-arg form does not disturb the sizeof fold.
TEST(SemanticAnalyzerCSubset, StaticAssertSizeof1ArgFolds) {
    auto cu = buildShippedUnit("c-subset", {
        "static_assert(sizeof(int) == 4);\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// ── C11/C23 6.5.3.4 _Alignof — the alignof-folding requirement ───────────────
//
// `_Static_assert(_Alignof(T)==N, ...)` const-evaluates the alignof through the
// SAME `constIntExpr` evaluator that folds sizeof — proving _Alignof is
// const-evaluable AND yields the EXACT alignment. Mirrors the sizeof pins above:
// a true alignof passes, a false one fails loud (not a rubber-stamp). Both
// spellings (`_Alignof`/`alignof`) and a struct type are exercised. The align
// resolver reads the SAME aggregateLayout params analyze() is given.
TEST(SemanticAnalyzerCSubset, StaticAssertAlignofIntFoldsTrue) {
    auto cu = buildShippedUnit("c-subset", {
        "_Static_assert(_Alignof(int) == 4, \"int aligns 4\");\n"
        "int main(void){ return 42; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "_Alignof(int)==4 must FOLD true (proves alignof is const-evaluable)";
}

TEST(SemanticAnalyzerCSubset, StaticAssertAlignofDoubleFoldsTrue) {
    auto cu = buildShippedUnit("c-subset", {
        "_Static_assert(_Alignof(double) == 8, \"double aligns 8\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "_Alignof(double)==8 must fold true";
}

// The C23 `alignof` spelling folds to alignment 1 for char.
TEST(SemanticAnalyzerCSubset, StaticAssertAlignofCharSpellingFoldsTrue) {
    auto cu = buildShippedUnit("c-subset", {
        "_Static_assert(alignof(char) == 1, \"char aligns 1\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "alignof(char)==1 must fold true (the C23 spelling)";
}

// _Alignof of a STRUCT = the MAX member alignment (not the size): {char; double}
// is 16 bytes but aligns to 8 (the double). Exercises the aggregateLayout path
// and proves alignof reads ALIGNMENT, never size.
TEST(SemanticAnalyzerCSubset, StaticAssertAlignofStructFoldsToMaxMemberAlign) {
    auto cu = buildShippedUnit("c-subset", {
        "struct CharDouble { char c; double d; };\n"
        "_Static_assert(_Alignof(struct CharDouble) == 8, \"aligns 8\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "_Alignof(struct{char;double;})==8 (max member align, NOT the size 16)";
}

TEST(SemanticAnalyzerCSubset, StaticAssertAlignofFoldsFalseFailsLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "_Static_assert(_Alignof(double) == 4, \"wrong on purpose\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 1u)
        << "_Alignof(double)==4 must FOLD false — the assertion fails loud "
           "(anti-rubber-stamp: the fold is real, and reads align not size)";
}

// ── C11/C23 6.7.5 _Alignas/alignas — alignment specifier ─────────────────────
//
// The FRONTEND + SEMANTICS: parse both spellings + both operand forms on a
// variable / struct-member, compute + validate the alignment, and STORE it
// (SymbolRecord.explicitAlignment for a variable; fed into the struct's
// fieldAligns for a member → computeLayout raises the layout end-to-end).
// D-CSUBSET-ALIGNAS. `analyze` is given the SAME aggregateLayout params the
// _Alignof pins use (Natural, stack-align 16) so member layout is exact.
namespace {
constexpr AggregateLayoutParams kAlignasLayout{ScalarAlignmentRule::Natural, 16};
}  // namespace

// PARSE: a global variable `alignas(16) int x;` (value form) parses cleanly —
// no parser diagnostics, one variable symbol.
TEST(SemanticAnalyzerCSubset, AlignasVariableValueFormParses) {
    auto cu = buildShippedUnit("c-subset", { "alignas(16) int x;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    SymbolRecord const* x = findSym(model, "x");
    ASSERT_NE(x, nullptr);
    EXPECT_TRUE(x->type.valid());
}

// PARSE: the TYPE operand form `alignas(double) int y;` parses (a type-name in
// the alignas operand contributes _Alignof(double)==8, which is ≥ int's 4, so
// no weaker-than-natural error).
TEST(SemanticAnalyzerCSubset, AlignasVariableTypeFormParses) {
    auto cu = buildShippedUnit("c-subset", { "alignas(double) int y;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasWeakerThanNatural), 0u);
    SymbolRecord const* y = findSym(model, "y");
    ASSERT_NE(y, nullptr);
    // alignas(double) = 8 on an int (natural 4) — a valid RAISE.
    ASSERT_TRUE(y->explicitAlignment.has_value());
    EXPECT_EQ(*y->explicitAlignment, 8u);
}

// PARSE: a struct member `struct S { alignas(16) int a; char b; };` parses.
TEST(SemanticAnalyzerCSubset, AlignasStructMemberParses) {
    auto cu = buildShippedUnit("c-subset",
                               { "struct S { alignas(16) int a; char b; };\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    // No alignas constraint diagnostics at all for a valid raise.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasInvalidContext), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasWeakerThanNatural), 0u);
}

// VARIABLE STORAGE: `alignas(32) int g;` sets SymbolRecord.explicitAlignment==32.
// (The stored value is intentionally NOT consumed by variable codegen yet — that
// is a separate deferred task; here we assert only that the SEMANTIC store works.)
TEST(SemanticAnalyzerCSubset, AlignasVariableStoresExplicitAlignment) {
    auto cu = buildShippedUnit("c-subset", { "alignas(32) int g;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    SymbolRecord const* g = findSym(model, "g");
    ASSERT_NE(g, nullptr);
    ASSERT_TRUE(g->explicitAlignment.has_value())
        << "alignas(32) must set SymbolRecord.explicitAlignment";
    EXPECT_EQ(*g->explicitAlignment, 32u);
}

// VALUE-EXPR STORAGE: `alignas(2*8) int g;` const-folds the operand to 16.
TEST(SemanticAnalyzerCSubset, AlignasVariableConstExprOperandFolds) {
    auto cu = buildShippedUnit("c-subset", { "alignas(2*8) int g;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    SymbolRecord const* g = findSym(model, "g");
    ASSERT_NE(g, nullptr);
    ASSERT_TRUE(g->explicitAlignment.has_value());
    EXPECT_EQ(*g->explicitAlignment, 16u);
}

// MEMBER LAYOUT END-TO-END (via the interner): `struct S { alignas(16) char c; }`
// → _Alignof(struct S)==16 AND sizeof(struct S)==16 (the alignas raised BOTH the
// struct's alignment and its rounded size). Reuses the _Static_assert(_Alignof())
// fold — RED-ON-DISABLE: without the fieldAligns wiring the struct aligns to 1.
TEST(SemanticAnalyzerCSubset, AlignasMemberRaisesStructAlignAndSizeEndToEnd) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { alignas(16) char c; };\n"
        "_Static_assert(_Alignof(struct S) == 16, \"aligns 16\");\n"
        "_Static_assert(sizeof(struct S) == 16, \"sizes 16\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "alignas(16) on the sole char member must raise the struct to "
           "_Alignof==16 AND sizeof==16 (end-to-end via fieldAligns)";
}

// MEMBER LAYOUT — following-field OFFSET: `struct T { char c; alignas(8) int i; }`
// pushes `i` from its natural offset 4 to 8. Proven via the _Alignof of the
// struct (max member align == 8) plus its size: char(1)+pad(7)+int(4) rounded to
// 8 → 16. (The offsetof idiom itself is exercised in the corpus/e2e probe.)
TEST(SemanticAnalyzerCSubset, AlignasMemberRaisesFollowingFieldLayout) {
    auto cu = buildShippedUnit("c-subset", {
        "struct T { char c; alignas(8) int i; };\n"
        "_Static_assert(_Alignof(struct T) == 8, \"aligns 8\");\n"
        "_Static_assert(sizeof(struct T) == 16, \"sizes 16\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// MEMBER LAYOUT — UNION: `union U { alignas(16) char c; int i; }` raises the
// union to _Alignof==16 and sizeof==16 (the completed carrier + the union-arm
// alignas fold in computeLayout).
TEST(SemanticAnalyzerCSubset, AlignasUnionMemberRaisesAlignAndSizeEndToEnd) {
    auto cu = buildShippedUnit("c-subset", {
        "union U { alignas(16) char c; int i; };\n"
        "_Static_assert(_Alignof(union U) == 16, \"aligns 16\");\n"
        "_Static_assert(sizeof(union U) == 16, \"sizes 16\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// ── D-CSUBSET-PACKED: `__attribute__((packed))` / `[[gnu::packed]]` semantics ──
// End-to-end via `_Static_assert(sizeof/_Alignof)`: the grammar parses the trailing
// composite-attribute list, the semantic scan marks the composite packed, the
// interner carries it, and computeLayout removes all padding. Each sizeof pin is
// RED-ON-DISABLE (a non-honored packed → the padded size → S_StaticAssertFailed).

// GNU spelling: `struct S {char c; int v;} __attribute__((packed));` → sizeof 5,
// _Alignof 1 (all inter-field padding removed, natural alignment 1).
TEST(SemanticAnalyzerCSubset, PackedStructGnuRemovesPaddingEndToEnd) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { char c; int v; } __attribute__((packed));\n"
        "_Static_assert(sizeof(struct S) == 5, \"packed size 5\");\n"
        "_Static_assert(_Alignof(struct S) == 1, \"packed align 1\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "packed must remove padding: sizeof==5 AND _Alignof==1 end-to-end";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u);
}

// C23 spelling: `[[gnu::packed]]` as a trailing attribute is honored identically.
TEST(SemanticAnalyzerCSubset, PackedStructC23GnuPackedSpelling) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { char c; int v; } [[gnu::packed]];\n"
        "_Static_assert(sizeof(struct S) == 5, \"packed size 5\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// C23 bare `[[packed]]` spelling (no namespace) is honored too.
TEST(SemanticAnalyzerCSubset, PackedStructC23BarePackedSpelling) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { char c; int v; } [[packed]];\n"
        "_Static_assert(sizeof(struct S) == 5, \"packed size 5\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// packed + a member `alignas` — alignas WINS per-field even under packed:
// `struct S {char c; alignas(4) int v;} __attribute__((packed));` → v@4, sizeof 8.
TEST(SemanticAnalyzerCSubset, PackedStructMemberAlignasStillRaises) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { char c; alignas(4) int v; } __attribute__((packed));\n"
        "_Static_assert(_Alignof(struct S) == 4, \"alignas wins\");\n"
        "_Static_assert(sizeof(struct S) == 8, \"v raised to offset 4\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "a member alignas raises per-field even inside a packed struct";
}

// `alignas(1)` INSIDE a packed struct is LEGAL (the member's natural baseline is 1
// under packed), so NO S_AlignasWeakerThanNatural. Contrast:
// `AlignasWeakerThanNaturalFailsLoud` — `alignas(1) double d;` OUTSIDE a packed
// struct still fails. RED-ON-DISABLE: drop the packed naturalBaseline and this fires.
TEST(SemanticAnalyzerCSubset, AlignasOneInsidePackedStructIsLegal) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { char c; alignas(1) int v; } __attribute__((packed));\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasWeakerThanNatural), 0u)
        << "alignas(1) inside a packed struct is legal (baseline 1)";
    EXPECT_FALSE(model.hasErrors());
}

// packed UNION: `union U {char c; int i;} __attribute__((packed));` → _Alignof 1.
TEST(SemanticAnalyzerCSubset, PackedUnionHasAlignmentOneEndToEnd) {
    auto cu = buildShippedUnit("c-subset", {
        "union U { char c; int i; } __attribute__((packed));\n"
        "_Static_assert(_Alignof(union U) == 1, \"packed union align 1\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// A trailing packed attribute does NOT block a following declarator:
// `struct S {...} __attribute__((packed)) g;` — `g` still parses, S stays packed.
TEST(SemanticAnalyzerCSubset, PackedStructFollowedByDeclaratorParses) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { char c; int v; } __attribute__((packed)) g;\n"
        "_Static_assert(sizeof(struct S) == 5, \"packed size 5\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
    EXPECT_NE(findSym(model, "g"), nullptr);
}

// FAIL-LOUD: packed + a bit-field member → S_PackedBitfieldUnsupported (bit-granular
// packed packing is a distinct, deferred algorithm — D-CSUBSET-PACKED-BITFIELD-
// INTERACTION). NEVER a silent NON-packed layout.
TEST(SemanticAnalyzerCSubset, PackedBitfieldFailsLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int a : 3; } __attribute__((packed));\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_PackedBitfieldUnsupported), 1u);
    EXPECT_TRUE(model.hasErrors());
}

// FAIL-LOUD: a TYPO in the GNU `__attribute__` packed slot → S_UnknownTypeAttribute
// (typo protection, like H_UnknownLinkageSpecifier — a `pakced` typo must not
// silently leave the struct unpacked).
TEST(SemanticAnalyzerCSubset, UnknownGnuTypeAttributeFailsLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int x; } __attribute__((pakced));\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 1u);
    EXPECT_TRUE(model.hasErrors());
}

// STANDARD-IGNORABLE: an unrecognized C23 `[[...]]` attribute on a struct is
// ignored (C23 6.7.11.1 — an unknown attribute is ignored), NO diagnostic. This is
// the `[[deprecated]]` precedent; only `packed`/`gnu::packed` are honored-or-diagnosed.
TEST(SemanticAnalyzerCSubset, UnknownC23AttributeIsIgnored) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int x; } [[deprecated]];\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// D-CSUBSET-PACKED-AFTER-KEYWORD-POSITION (F-1): the AFTER-KEYWORD packed position
// `struct __attribute__((packed)) S {…}` is DEFERRED — only the TRAILING/suffix form
// (`struct S {…} __attribute__((packed))`) is honored. That deferral's fail-loud
// contract REQUIRES the after-keyword form to be LOUDLY REJECTED — NEVER a silent
// tag-drop / accept-as-unpacked. The grammar admits `compositeAttrList` only as a
// TRAILING element, so `struct __attribute__((packed))` parses (cleanly, no
// tree-builder error) as an ANONYMOUS, body-less struct specifier — the attribute is
// consumed as ITS trailing list — and the tag `S { … }` is then mis-read as a
// function definition, failing loud S_InvalidFunctionDeclarator at SEMANTIC analysis
// (verified: this is a semantic, not a parse, error). The pin is PHASE-AGNOSTIC
// (tree parse-error OR semantic-model error) so a future shift between channels stays
// green; ONLY a silent accept-as-unpacked — the regression F-1 guards against, an
// after-keyword slot added WITHOUT fixing the ~6 positional tag readers — turns it
// red. RED-ON-REGRESSION.
TEST(SemanticAnalyzerCSubset, PackedAfterKeywordTaggedFailsLoudNotSilent) {
    auto cu = buildShippedUnit("c-subset", {
        "struct __attribute__((packed)) S { int a; };\n"
        "int main(void){ return 0; }\n",
    });
    bool sawParseError = false;
    for (auto const& t : cu->trees()) {
        for (auto const& d : t.diagnostics().all()) {
            if (d.severity == DiagnosticSeverity::Error) sawParseError = true;
        }
    }
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_TRUE(sawParseError || model.hasErrors())
        << "after-keyword packed (`struct __attribute__((packed)) S {…}`) must fail "
           "LOUD (parse or semantic) — never a silent tag-drop / accept-as-unpacked "
           "(the D-CSUBSET-PACKED-AFTER-KEYWORD-POSITION deferral's fail-loud contract)";
}

// The ANONYMOUS after-keyword variant is likewise LOUDLY REJECTED:
// `struct __attribute__((packed)) { int a; } v;` — the anonymous, body-less struct
// specifier consumes the attribute as its trailing list, then `{ int a; } v;` cannot
// bind and fails loud (S_UnknownType on `v` today). Cheap sibling; same fail-loud
// boundary, phase-agnostic.
TEST(SemanticAnalyzerCSubset, PackedAfterKeywordAnonymousFailsLoudNotSilent) {
    auto cu = buildShippedUnit("c-subset", {
        "struct __attribute__((packed)) { int a; } v;\n"
        "int main(void){ return 0; }\n",
    });
    bool sawParseError = false;
    for (auto const& t : cu->trees()) {
        for (auto const& d : t.diagnostics().all()) {
            if (d.severity == DiagnosticSeverity::Error) sawParseError = true;
        }
    }
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_TRUE(sawParseError || model.hasErrors())
        << "anonymous after-keyword packed must also fail LOUD (parse or semantic), "
           "never a silent accept-as-unpacked";
}

// ZERO: `alignas(0) int x;` is a NO-OP (6.7.5p3) — NO diagnostic, NO override.
TEST(SemanticAnalyzerCSubset, AlignasZeroIsNoOpNoOverride) {
    auto cu = buildShippedUnit("c-subset", { "alignas(0) int x;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNotPowerOfTwo), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNonConstant), 0u);
    SymbolRecord const* x = findSym(model, "x");
    ASSERT_NE(x, nullptr);
    EXPECT_FALSE(x->explicitAlignment.has_value())
        << "alignas(0) has no effect — no override stored";
}

// CONSTRAINT: a non-power-of-two value → S_AlignasNotPowerOfTwo.
TEST(SemanticAnalyzerCSubset, AlignasNotPowerOfTwoFailsLoud) {
    auto cu = buildShippedUnit("c-subset", { "alignas(3) int x;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNotPowerOfTwo), 1u);
}

// CONSTRAINT: a value over the 256-byte cap → S_AlignasExceedsMax (a distinct
// code from not-power-of-two — 512 IS a power of two, just too large).
TEST(SemanticAnalyzerCSubset, AlignasExceedsMaxFailsLoud) {
    auto cu = buildShippedUnit("c-subset", { "alignas(512) int x;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasExceedsMax), 1u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNotPowerOfTwo), 0u);
}

// CONSTRAINT: an alignment WEAKER than the declared type's natural alignment →
// S_AlignasWeakerThanNatural (6.7.5p4: alignas may only strengthen; 1 < 8).
TEST(SemanticAnalyzerCSubset, AlignasWeakerThanNaturalFailsLoud) {
    auto cu = buildShippedUnit("c-subset", { "alignas(1) double d;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasWeakerThanNatural), 1u);
}

// CONSTRAINT: a non-constant value operand → S_AlignasNonConstant.
TEST(SemanticAnalyzerCSubset, AlignasNonConstantFailsLoud) {
    auto cu = buildShippedUnit("c-subset",
                               { "int nc; alignas(nc) int x;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNonConstant), 1u);
}

// CONSTRAINT: a NEGATIVE value is a constraint violation (NOT the 6.7.5p3 zero
// no-op) → S_AlignasNotPowerOfTwo. Fail-loud: `alignas(-4)` must NOT be silently
// swallowed as "no alignment" (a negative is not a valid alignment; gcc/clang
// both reject it). RED-ON-DISABLE: were `value <= 0` treated as a no-op, this
// would compile with ZERO diagnostics — a silent constraint violation.
TEST(SemanticAnalyzerCSubset, AlignasNegativeFailsLoud) {
    auto cu = buildShippedUnit("c-subset", { "alignas(-4) int x;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNotPowerOfTwo), 1u)
        << "alignas(-4) is a constraint violation, not a no-op — fail loud";
}

// EXACTLY-ONE diagnostic across a MULTI-DECLARATOR declaration: the alignas lives
// on the shared prefix, so `alignas(3) int a, b;` is ONE erroneous specifier →
// ONE S_AlignasNotPowerOfTwo (not one per declarator). RED-ON-DISABLE for the
// per-declaration emit gate: without it the diagnostic fires twice.
TEST(SemanticAnalyzerCSubset, AlignasMultiDeclaratorEmitsExactlyOnce) {
    auto cu = buildShippedUnit("c-subset", { "alignas(3) int a, b;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNotPowerOfTwo), 1u)
        << "a shared-prefix alignas error must be reported once, not per declarator";
}

// MULTI-DECLARATOR STORE: a VALID `alignas(16) int a, b;` stores the override on
// EVERY declarator's symbol (the prefix applies to all slots).
TEST(SemanticAnalyzerCSubset, AlignasMultiDeclaratorStoresOnAll) {
    auto cu = buildShippedUnit("c-subset", { "alignas(16) int a, b;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    SymbolRecord const* a = findSym(model, "a");
    SymbolRecord const* b = findSym(model, "b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_TRUE(a->explicitAlignment.has_value());
    ASSERT_TRUE(b->explicitAlignment.has_value());
    EXPECT_EQ(*a->explicitAlignment, 16u);
    EXPECT_EQ(*b->explicitAlignment, 16u);
}

// CONSTRAINT: alignas on a FUNCTION declaration → S_AlignasInvalidContext.
TEST(SemanticAnalyzerCSubset, AlignasOnFunctionFailsLoud) {
    auto cu = buildShippedUnit("c-subset",
                               { "alignas(16) int f(void);\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasInvalidContext), 1u);
}

// CONSTRAINT: alignas on a BIT-FIELD member → S_AlignasInvalidContext (6.7.5p2).
TEST(SemanticAnalyzerCSubset, AlignasOnBitFieldMemberFailsLoud) {
    auto cu = buildShippedUnit("c-subset",
                               { "struct S { alignas(8) int a : 3; };\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasInvalidContext), 1u);
}

// The C11 spelling `_Alignas` works identically to the C23 `alignas`.
TEST(SemanticAnalyzerCSubset, AlignasC11SpellingStores) {
    auto cu = buildShippedUnit("c-subset", { "_Alignas(64) int g;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    SymbolRecord const* g = findSym(model, "g");
    ASSERT_NE(g, nullptr);
    ASSERT_TRUE(g->explicitAlignment.has_value());
    EXPECT_EQ(*g->explicitAlignment, 64u);
}

// VALUE-EXPR with an ENUM CONSTANT operand: `alignas(W)` where `W` is an enum
// constant folds to 16. RED-ON-DISABLE for the type-vs-value discrimination: a
// bare non-typedef identifier (`W`) must roll back to the VALUE reading and
// const-fold (the `requireKnownType` polarity) — under the PreferType default it
// would wrongly commit as a type-name and emit a spurious S_AlignasNonConstant.
TEST(SemanticAnalyzerCSubset, AlignasEnumConstantOperandFolds) {
    auto cu = buildShippedUnit("c-subset", {
        "enum E { W = 16 };\n"
        "alignas(W) int g;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNonConstant), 0u)
        << "an enum-constant alignas operand must roll back to the VALUE reading "
           "and fold (requireKnownType), not commit as a type-name";
    SymbolRecord const* g = findSym(model, "g");
    ASSERT_NE(g, nullptr);
    ASSERT_TRUE(g->explicitAlignment.has_value());
    EXPECT_EQ(*g->explicitAlignment, 16u);
}

// VALUE-EXPR with a sizeof operand: `alignas(sizeof(double))` folds to 8 (the
// alignas value-form operand runs through the SAME sizeof-folding constIntExpr).
TEST(SemanticAnalyzerCSubset, AlignasSizeofOperandFolds) {
    auto cu = buildShippedUnit("c-subset", { "alignas(sizeof(double)) int g;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    SymbolRecord const* g = findSym(model, "g");
    ASSERT_NE(g, nullptr);
    ASSERT_TRUE(g->explicitAlignment.has_value());
    EXPECT_EQ(*g->explicitAlignment, 8u);
}

// ── FC17 C23 6.7.2.5 typeof / typeof_unqual ─────────────────────────────────
//
// ★ THE red-on-disable pin for the CRITICAL scan-opacity fix: `typeof_unqual`'s
// SOLE observable effect is stripping the top-level qualifier. Preservation
// passes with-or-without the qualifier-scan-leak bug (both leave volatile on),
// so ONLY the strip case catches a regression — if the coarse base-volatile scan
// descends into the typeof operand and re-applies the literal `volatile` AFTER
// the arm stripped it, `v` would come back `volatile int` and this FAILS.
TEST(SemanticAnalyzerCSubset, TypeofUnqualStripsVolatile) {
    auto cu = buildShippedUnit("c-subset", {
        "typeof_unqual(volatile int) v;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->type.valid());
    EXPECT_FALSE(ti.isVolatileQualified(v->type))
        << "typeof_unqual strips the top-level volatile — the coarse volatile "
           "scan must NOT re-apply the operand's literal `volatile`";
    EXPECT_EQ(ti.kind(v->type), TypeKind::I32)
        << "the stripped type is bare int";
}

// The KEPT side: `typeof` PRESERVES the top-level qualifier (VolatileQual(int)).
TEST(SemanticAnalyzerCSubset, TypeofPreservesVolatile) {
    auto cu = buildShippedUnit("c-subset", {
        "typeof(volatile int) v;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->type.valid());
    EXPECT_TRUE(ti.isVolatileQualified(v->type))
        << "typeof (not typeof_unqual) keeps the top-level volatile";
}

// Fork-B polarity pin: `typeof(ENUM_CONSTANT)` must ROLL BACK to the VALUE form
// (requireKnownType) and type as an expression — NOT commit the enum constant as
// a type-name → a spurious S_UnknownType. Mirrors AlignasEnumConstantOperandFolds.
TEST(SemanticAnalyzerCSubset, TypeofEnumConstantOperandResolvesAsValue) {
    auto cu = buildShippedUnit("c-subset", {
        "enum E { GREEN = 7 };\n"
        "typeof(GREEN) v;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownType), 0u)
        << "an enum-constant typeof operand must roll back to the VALUE reading "
           "(requireKnownType), not commit as a type-name";
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    EXPECT_TRUE(v->type.valid())
        << "typeof(GREEN) resolves to the enum constant's type";
}

// EXPRESSION form: `typeof(x)` for a declared `x` resolves to x's type.
TEST(SemanticAnalyzerCSubset, TypeofExpressionFormResolvesToOperandType) {
    auto cu = buildShippedUnit("c-subset", {
        "int x;\n"
        "typeof(x) y;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* y = findSym(model, "y");
    ASSERT_NE(y, nullptr);
    ASSERT_TRUE(y->type.valid());
    EXPECT_EQ(ti.kind(y->type), TypeKind::I32)
        << "typeof(x) where x is int must resolve y to int";
}

// TYPE-NAME form: `typeof(int*)` resolves to Ptr<int> (castTypeRef operand).
TEST(SemanticAnalyzerCSubset, TypeofTypeNameFormResolvesPointer) {
    auto cu = buildShippedUnit("c-subset", {
        "typeof(int*) p;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* p = findSym(model, "p");
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(p->type.valid());
    ASSERT_EQ(ti.kind(p->type), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(ti.operands(p->type)[0]), TypeKind::I32);
}

// `sizeof(typeof(unsigned short))` folds to 2 in an array dimension — the typeof
// resolves inside the SAME sizeof-fold path sizeof(T)/enum/arithmetic use.
TEST(SemanticAnalyzerCSubset, SizeofTypeofFoldsInArrayDim) {
    auto cu = buildShippedUnit("c-subset", {
        "int a[sizeof(typeof(unsigned short))];\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64, kAlignasLayout);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* a = findSym(model, "a");
    ASSERT_NE(a, nullptr);
    ASSERT_TRUE(a->type.valid());
    ASSERT_EQ(ti.kind(a->type), TypeKind::Array);
    ASSERT_EQ(ti.scalars(a->type).size(), 1u);
    EXPECT_EQ(ti.scalars(a->type)[0], 2)
        << "sizeof(typeof(unsigned short)) folds to 2";
}

// Bit-field operand → S_TypeofBitfieldOperand (C 6.7.2.5 constraint): a bit-field
// has no nameable type. RED-on-disable for the bit-field gate.
TEST(SemanticAnalyzerCSubset, TypeofBitfieldOperandFailsLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { unsigned f : 3; };\n"
        "struct S s;\n"
        "typeof(s.f) v;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeofBitfieldOperand), 1u)
        << "typeof of a bit-field member is a constraint violation";
}

// ── FC16 C11/C23 6.5.1.1 _Generic — generic selection ────────────────────────
//
// SELECTION is a compile-time SEMANTIC-tier decision (like sizeof folding): the
// controlling expression's type is matched against each association's resolved
// type-name; the WINNER's result type is stamped on the genericExpr node (so the
// enclosing expression types), and a no-match/ambiguous/value-in-type failure is
// fail-loud. These pins prove the selection is REAL (the RESULT TYPE follows the
// SELECTED association — an int-controlled `_Generic` picking an `int:` branch
// that yields a `double` types the node `double`, not `int`).
namespace {
// The first genericExpr node across the CU's trees (the whole `_Generic (...)`
// primary expression), for the RESULT-TYPE stamp checks.
[[nodiscard]] std::pair<TreeId, NodeId> firstGenericNode(CompilationUnit const& cu) {
    for (auto const& t : cu.trees()) {
        auto const rid = t.schema().rules().find("genericExpr");
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

// The selected association's TYPE is the `_Generic` node's result type. `i` is
// `int`, so the `int:` association wins; its result expression is a `double`
// literal — so the genericExpr node types `double` (F64), NOT `int`. This is the
// load-bearing behavior: the result type follows the SELECTED branch's value.
TEST(SemanticAnalyzerCSubset, GenericSelectedBranchTypeIsResultType) {
    auto cu = buildShippedUnit("c-subset", {
        "double f(void){ int i = 0; return _Generic(i, int: 1.5, "
        "long: 2, default: 0); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    auto [tid, gen] = firstGenericNode(*cu);
    ASSERT_TRUE(gen.valid()) << "a genericExpr node must exist";
    TypeId const genTy = model.typeAt(gen);
    ASSERT_TRUE(genTy.valid()) << "the _Generic node must be typed (selection ok)";
    EXPECT_EQ(ti.kind(genTy), TypeKind::F64)
        << "the result type is the SELECTED int-branch's double value (F64)";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_GenericSelectionNoMatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_GenericSelectionAmbiguous), 0u);
}

// A controlling type matching NO typed association AND no default fails loud
// (S_GenericSelectionNoMatch) — `double` vs {int, char}.
TEST(SemanticAnalyzerCSubset, GenericNoMatchNoDefaultFailsLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(void){ double d = 0; return _Generic(d, int: 1, char: 2); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_GenericSelectionNoMatch), 1u)
        << "no typed match and no default is a constraint violation";
}

// The `default` fallback is selected when no typed association matches — `char*`
// vs {int, double} → default. No no-match error; the node types the default's
// result type.
TEST(SemanticAnalyzerCSubset, GenericDefaultFallbackSelected) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(void){ char c = 0; char* p = &c; "
        "return _Generic(p, int: 1, double: 2, default: 7); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_GenericSelectionNoMatch), 0u)
        << "the default association must satisfy an otherwise-no-match selection";
    auto [tid, gen] = firstGenericNode(*cu);
    ASSERT_TRUE(gen.valid());
    EXPECT_TRUE(model.typeAt(gen).valid())
        << "the default-selected _Generic node must be typed";
}

// A VALUE in an association's type position fails loud at the type-resolve — the
// castTypeRef `commitRequiresTypeName` triage routes a value-identifier to
// S_UnknownType (never silently treated as a type).
TEST(SemanticAnalyzerCSubset, GenericValueInTypePositionFailsLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(void){ int i = 0; int notAType = 5; "
        "return _Generic(i, notAType: 1, default: 0); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownType), 1u)
        << "a value identifier in an association type position must fail loud";
}

// Two associations naming the SAME type (compatible types — 6.5.1.1p2 forbids it)
// is ambiguous → S_GenericSelectionAmbiguous.
TEST(SemanticAnalyzerCSubset, GenericAmbiguousMatchFailsLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(void){ int i = 0; "
        "return _Generic(i, int: 1, int: 2, default: 0); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_GenericSelectionAmbiguous), 1u)
        << "two associations of the same type is a constraint violation";
}

// A typedef name in an association type position resolves through the alias and
// matches the underlying type (`MyInt` ≡ `int` → the MyInt-branch wins for an
// `int` controlling expression).
TEST(SemanticAnalyzerCSubset, GenericTypedefAssociationMatches) {
    auto cu = buildShippedUnit("c-subset", {
        "typedef int MyInt;\n"
        "int main(void){ int i = 0; "
        "return _Generic(i, MyInt: 42, double: 3, default: 0); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_GenericSelectionNoMatch), 0u)
        << "a typedef alias in type position must match the underlying type";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_GenericSelectionAmbiguous), 0u);
}

// R1: the GENUINE forward-reference case (`int a[sizeof(b)]; int b;` — b used
// before its declaration's type resolves) STAYS correct fail-loud (invalid C:
// declare-before-use). This is NOT the closed member-access case — it pins the
// reclassified anchor: forward-ref rejected, member-access-at-Pass-1.5 closed.
// VLA C1a (D-CSUBSET-VLA): pinned at FILE scope so a non-foldable sizeof operand
// stays S_NonConstantArrayLength (a file-scope array needs a constant bound — it is
// NOT a VLA). Block-scope `int a[sizeof(b)]` would be a VLA (accepted at semantic,
// fails at the LIR C1b boundary); the const-eval-refusal intent is preserved here.
TEST(SemanticAnalyzerCSubset, ForwardRefSizeofArrayDimensionStillRejected) {
    auto cu = buildShippedUnit("c-subset", {
        "int a[sizeof(b)]; int b;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 1u)
        << "a forward-referenced sizeof operand must fail loud, never fold";
}

// R1: a non-existent field in the sizeof operand fails loud. VLA C1a
// (D-CSUBSET-VLA): the array dim is now a block-scope VLA (accepted), but the
// UNDERLYING bad-field access `s.nope` fails loud on its own
// (S_UndeclaredIdentifier) — the build still fails, never a silently-folded guessed
// size. Guards against the member arm admitting a phantom field.
TEST(SemanticAnalyzerCSubset, BadFieldSizeofArrayDimensionRejected) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int x; int y; };\n"
        "int main() { struct S s; int a[sizeof(s.nope)]; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_TRUE(model.hasErrors())
        << "sizeof(s.nope) — no such field — must fail loud, never fold a guess";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UndeclaredIdentifier), 1u)
        << "the phantom field `nope` fails loud independently of the array dim";
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
    // FC12a-core builtin TYPES (__va_list_tag + va_list) + the 5 intrinsic
    // builtins (c103 __umulh + c104 _InterlockedCompareExchange + c113
    // _ReadWriteBarrier + c115 _exception_code + _exception_info) + the 6
    // FC17.9(b) bit-count builtins (__builtin_{popcount,clz,ctz}{,ll},
    // D-CSUBSET-BITCOUNT-INTRINSICS) + the 56 FC17.9(b) <stdbit.h>
    // __builtin_stdc_<op>_<T> intrinsics (14 ops × 4 widths, D-FULLC-STDBIT) +
    // the 2 FC17.9(d) atomic accessors (atomic_load_explicit + atomic_store_explicit,
    // D-CSUBSET-ATOMIC) + the 4 FC17.9(f) complex builtins (__builtin_complex/creal/
    // cimag/conj, D-CSUBSET-COMPLEX) + the 2 FC17.5 predefined function-name symbols
    // (__func__ + __FUNCTION__, per function definition — D-CSUBSET-FUNC-PREDEFINED-IDENTIFIER).
    EXPECT_EQ(model.symbols().size() - 1, 80u);
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
    // (__va_list_tag + va_list) + the 5 intrinsic builtins (c103 __umulh +
    // c104 _InterlockedCompareExchange + c113 _ReadWriteBarrier + c115
    // _exception_code + _exception_info) + the 6 FC17.9(b) bit-count builtins
    // (__builtin_{popcount,clz,ctz}{,ll}, D-CSUBSET-BITCOUNT-INTRINSICS) + the 56
    // FC17.9(b) <stdbit.h> __builtin_stdc_<op>_<T> intrinsics (D-FULLC-STDBIT) +
    // the 2 FC17.9(d) atomic accessors (atomic_load_explicit + atomic_store_explicit,
    // D-CSUBSET-ATOMIC) + the 4 FC17.9(f) complex builtins (__builtin_complex/creal/
    // cimag/conj, D-CSUBSET-COMPLEX) + the 2 FC17.5 predefined function-name symbols
    // (__func__ + __FUNCTION__). Find x by name.
    ASSERT_EQ(model.symbols().size() - 1, 79u);
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

// D-CSUBSET-FOR-INIT-SCOPE (C99 6.8.5.3): each `for`-statement's init clause has its
// OWN scope, so two SIBLING `for (int i = ...)` loops in one block re-declaring the same
// loop name are BOTH valid — the second `i` is a distinct object in a distinct scope,
// not a redeclaration of the first. Before the fix (`forStmt` absent from the config
// `scopes` list) the for-init leaked into the enclosing block, so the second `for(int i)`
// mis-resolved: its uses reported undeclared AND its decl reported unused. Red-on-disable:
// revert the `scopes` add and BOTH diagnostics fire on the second loop.
TEST(SemanticAnalyzerCSubset, SiblingForInitSameNameHaveDistinctScopes) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(void){\n"
        "  int s = 0;\n"
        "  for (int i = 0; i < 2; i++) s = s + i;\n"
        "  for (int i = 0; i < 2; i++) s = s + i;\n"   // same name — a distinct for-scope
        "  return s;\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "the second for(int i)'s uses must resolve to its own for-scoped i";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u)
        << "the second for(int i)'s decl must not be orphaned as unused";
}

// D-CSUBSET-FOR-INIT-SCOPE control: the for-init variable is OUT of scope AFTER the
// for-statement (it is a real for-scope, not a leak into the enclosing block). Using `i`
// after the loop must fail loud (S_UndeclaredIdentifier) — this is what proves the fix is
// a correct scope, and it stays a fail-loud reject, never a silent resolve to a stale i.
TEST(SemanticAnalyzerCSubset, ForInitVariableOutOfScopeAfterForRejects) {
    auto cu = buildShippedUnit("c-subset", {
        "int main(void){ for (int i = 0; i < 2; i++) {} return i; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 1u)
        << "`i` is scoped to the for-statement — a use after the loop must fail loud";
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

// ── Cycle D — C11/C23 6.4.5p5: adjacent-concat prefix MIXING (semantic typing) ─
// The run's element core is keyed by its EFFECTIVE prefix (the single distinct
// non-narrow opener among ALL segments), NOT the first opener. Two DIFFERENT
// non-narrow prefixes leave the node UNTYPED + emit H_ConflictingStringLiteralPrefixes.

// THE typing defect fix: `"a" L"b"` — first opener narrow, but the run's effective
// prefix is L (wchar_t) so the WHOLE literal types Array<wchar_t, 3> (I32 on the
// POSIX default), the narrow "a" widened. RED-ON-DISABLE: first-opener keying stamps
// Array<Char,3> here — the semantic/HIR mistype the two tiers would AGREE on wrongly.
TEST(SemanticAnalyzerCSubset, ConcatEffectivePrefixTypesWholeWideArray) {
    auto cu = buildShippedUnit("c-subset", { "void f(){ \"a\" L\"b\"; }" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    ASSERT_FALSE(model.hasErrors());
    auto const& ti = model.lattice().interner();
    TypeId const ty = firstStringLiteralType(model, *cu);
    ASSERT_TRUE(ty.valid());
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::I32)
        << "`\"a\" L\"b\"` — the trailing L prefix wins (was silently dropped)";
    EXPECT_EQ(ti.scalars(ty)[0], 3) << "'a' widened + 'b' + wide NUL";
}

// Same-prefix `u"a" u"b"` (one distinct non-narrow kind) is NOT a conflict →
// Array<U16,3>, existing behavior.
TEST(SemanticAnalyzerCSubset, ConcatSamePrefixTypesU16) {
    auto cu = buildShippedUnit("c-subset", { "void f(){ u\"a\" u\"b\"; }" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    ASSERT_FALSE(model.hasErrors());
    auto const& ti = model.lattice().interner();
    TypeId const ty = firstStringLiteralType(model, *cu);
    ASSERT_TRUE(ty.valid());
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::U16);
    EXPECT_EQ(ti.scalars(ty)[0], 3);
}

// MF1 / N6: a run mixing two DIFFERENT non-narrow prefixes leaves the rule node
// UNTYPED and emits H_ConflictingStringLiteralPrefixes at the SEMANTIC tier (so a
// `sizeof` of it reports the real reason, not a bare sizeof-of-untyped cascade).
TEST(SemanticAnalyzerCSubset, ConcatConflictLeavesNodeUntypedAndEmits) {
    auto cu = buildShippedUnit("c-subset", { "void f(){ u\"a\" U\"b\"; }" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_TRUE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::H_ConflictingStringLiteralPrefixes), 1u);
    TypeId const ty = firstStringLiteralType(model, *cu);
    EXPECT_FALSE(ty.valid())
        << "a mixed-prefix run is left UNTYPED so a sizeof of it fails loud";
}

// N6: `sizeof(u"a" U"b")` fails loud with the conflict reason (not a silent fold /
// bare sizeof-of-untyped). The conflict is emitted once, on the rule node.
TEST(SemanticAnalyzerCSubset, ConcatConflictSizeofFailsLoud) {
    auto cu = buildShippedUnit("c-subset", { "int f(){ return sizeof(u\"a\" U\"b\"); }" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_TRUE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::H_ConflictingStringLiteralPrefixes), 1u);
}

// MF2 (the AGNOSTICISM witness): the conflict compares opener TOKEN KINDS, never
// resolved cores. `u"a" L"b"` mixes `u"` (char16_t) and `L"` (wchar_t) — two
// DIFFERENT non-narrow token kinds → conflict on EVERY target. On pe both resolve
// to U16 (SAME core), so a core-keyed check would silently ACCEPT it on Windows
// while rejecting it on Linux (I32 ≠ U16). This asserts the reject on BOTH the
// default (elf, different cores) AND pe (same core) — RED-ON-DISABLE of a core-keyed
// classifier flips the pe arm to accept.
TEST(SemanticAnalyzerCSubset, ConcatConflictIsTokenKindNotCoreEvenOnPe) {
    for (bool pe : {false, true}) {
        auto cu = buildShippedUnit("c-subset", { "void f(){ u\"a\" L\"b\"; }" });
        assertNoBuilderErrors(*cu);
        auto model = pe ? analyze(cu, DataModel::Llp64, std::nullopt, std::nullopt,
                                  ObjectFormatKind::Pe)
                        : analyze(cu);
        EXPECT_TRUE(model.hasErrors()) << (pe ? "pe" : "default");
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::H_ConflictingStringLiteralPrefixes), 1u)
            << (pe ? "pe: u\"/L\" both resolve to U16 but the TOKEN KINDS differ"
                   : "default: u\"→U16 vs L\"→I32");
    }
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

// THE Part-B integration point: an IMPLICIT distinct-typed-pointer conversion in
// `int* f(char* p) { return p; }` is rejected (S_ReturnTypeMismatch — the strict
// no-silent-conversion bar), while the EXPLICIT `(int*)p` form is accepted: the
// cast node's result type is the stamped target (int*), which returns cleanly.
// Both directions in one test so the contrast is pinned, not assumed.
// (This test originally contrasted the implicit F64->I32 narrowing in
// `return 1.7+2.5;` against `(int)(1.7+2.5)`, but D-CSUBSET-INT-FLOAT-CONVERSION
// made float->int an ADMITTED implicit conversion in c-subset, so the implicit
// form no longer fires; a distinct-typed-pointer pair is the stable implicit-
// rejected / explicit-accepted contrast that still exercises the same FC2
// explicit-cast-vs-implicit-assignability split.)
TEST(SemanticAnalyzerCSubset, ExplicitPointerCastAcceptedWhereImplicitRejected) {
    auto implicitModel = analyzeShipped("c-subset", {
        "int* f(char* p) { return p; }\n",
    });
    EXPECT_EQ(countCode(implicitModel.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 1u)
        << "the implicit char* -> int* conversion must stay rejected";

    auto castModel = analyzeShipped("c-subset", {
        "int* f(char* p) { return (int*)p; }\n",
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

// ── c37 D-CSUBSET-FUNCTION-DESIGNATOR-CAST — `(fp)g` (function name -> fn-ptr) ──
// C 6.3.2.1p4 + 6.3.2.3p8: a function DESIGNATOR decays to the function's
// address; casting it to any fn-ptr type (even a DIFFERENT signature) is legal
// and value-preserving. The sqlite `(sqlite3_destructor_type)fn` /
// `(sqlite3_syscall_ptr)fn` shapes (29x). Red-on-disable: drop the
// `(Ptr && FnSig)` arm in isExplicitCastable and the two positive pins fire
// S_InvalidCast; the two negative pins guard against over-admission.

// Positive — same-signature function -> fn-ptr typedef (the SQLite pattern).
TEST(SemanticAnalyzerCSubset, FunctionDesignatorToFnPtrCastIsAccepted) {
    auto model = analyzeShipped("c-subset", {
        "typedef void(*dtor)(void*);\n"
        "void real(void* p){ (void)p; }\n"
        "int main(){ dtor d = (dtor)real; return d ? 0 : 1; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidCast), 0u)
        << "a function designator cast to its own fn-ptr type is legal C";
}

// Positive — CROSS-signature fn-ptr cast (C 6.3.2.3p8; sqlite syscall shapes).
TEST(SemanticAnalyzerCSubset, CrossSignatureFnPtrCastIsAccepted) {
    auto model = analyzeShipped("c-subset", {
        "typedef int(*fp)(int);\n"
        "void g(void* x){ (void)x; }\n"
        "int main(){ fp f = (fp)g; return f ? 0 : 1; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidCast), 0u)
        << "cross-signature fn-ptr cast is legal C (calling through it is UB, the cast is not)";
}

// Negative (over-admission guard) — STRUCT value -> fn-ptr stays REJECTED.
TEST(SemanticAnalyzerCSubset, StructToFnPtrCastStillRejected) {
    auto model = analyzeShipped("c-subset", {
        "typedef void(*fp)(void);\n"
        "struct S { int x; };\n"
        "int main(){ struct S s; s.x = 0; fp f = (fp)s; return f ? 0 : 1; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidCast), 1u)
        << "a struct value is not a function designator — must stay fail-loud";
}

// Negative (over-admission guard) — function designator -> INT stays REJECTED
// (the new arm is pointer-target-only: tk==Ptr).
TEST(SemanticAnalyzerCSubset, FunctionDesignatorToIntStillRejected) {
    auto model = analyzeShipped("c-subset", {
        "void g(void){}\n"
        "int main(){ long p = (long)g; return (int)p; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidCast), 1u)
        << "casting a function designator directly to an integer stays rejected";
}

// ── c38 D-CSUBSET-NESTED-TAG-SCOPE — a tag nested in a struct body has
// ENCLOSING (block/file) scope (C 6.2.1p4), not the inner struct's member
// scope. `floatToNamespaceScope` now floats a nested tag PAST the composite
// body. The single largest sqlite S000D class (WalSegment/sColMap/IdList_item).
// Red-on-disable: restore the composite-body `break` and c38a/e/f fail
// S_NotAComposite / S_IncompleteTypeObject.

// c38a — accept: a tag defined nested in Outer is visible BY NAME at file scope.
TEST(SemanticAnalyzerCSubset, NestedTagReferencedAtFileScopeComposes) {
    auto model = analyzeShipped("c-subset", {
        "struct Outer { struct Inner { int x; } m; };\n"
        "int f(struct Inner *p){ return p->x; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_IncompleteTypeObject), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u);
}

// c38b — accept: a value object of the nested tag works (it is COMPLETE at file scope).
TEST(SemanticAnalyzerCSubset, NestedTagValueObjectWorks) {
    auto model = analyzeShipped("c-subset", {
        "struct Outer { struct Inner { int x; } m; };\n"
        "int main(void){ struct Inner v; v.x = 5; return v.x; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_IncompleteTypeObject), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u);
}

// c38c — REGRESSION guard: member composition (`o->m.x`) must STILL work
// (the nested struct's member TYPE is resolved via structScope, independent of
// the tag BIND scope — the fix must not break this).
TEST(SemanticAnalyzerCSubset, NestedTagMemberAccessStillComposes) {
    auto model = analyzeShipped("c-subset", {
        "struct Outer { struct Inner { int x; } m; };\n"
        "int g(struct Outer *o){ return o->m.x; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u);
}

// c38d — OVER-FLOAT guard: a nested tag in a BLOCK-local struct floats only to
// the BLOCK scope, NOT file scope — a file-scope reference must STILL fail.
TEST(SemanticAnalyzerCSubset, NestedTagInBlockDoesNotLeakToFileScope) {
    auto model = analyzeShipped("c-subset", {
        "void f(void){ struct Outer { struct Inner { int x; } m; }; }\n"
        "int main(void){ struct Inner v; v.x = 0; return v.x; }\n",
    });
    // Inner is block-scoped to f's body; at file scope it is unknown/incomplete.
    EXPECT_GE(countCode(model.diagnostics(), DiagnosticCode::S_IncompleteTypeObject)
              + countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 1u);
}

// c38e — union nested tag at file scope.
TEST(SemanticAnalyzerCSubset, NestedUnionTagComposes) {
    auto model = analyzeShipped("c-subset", {
        "union U { struct Item { int v; } it; };\n"
        "int f(struct Item *p){ return p->v; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_IncompleteTypeObject), 0u);
}

// c38f — deeply nested (struct in struct in struct): the innermost tag floats
// all the way to file scope.
TEST(SemanticAnalyzerCSubset, DeeplyNestedTagComposes) {
    auto model = analyzeShipped("c-subset", {
        "struct A { struct B { struct C { int v; } c; } b; };\n"
        "int f(struct C *p){ return p->v; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_IncompleteTypeObject), 0u);
}

// c38g — shadowing: a nested tag of the same name as a FILE-scope tag both land
// in file scope → a redefinition collision (C 6.7p3 — one definition per scope).
TEST(SemanticAnalyzerCSubset, NestedTagShadowingFileScopeTagCollides) {
    auto model = analyzeShipped("c-subset", {
        "struct Inner { int a; };\n"
        "struct Outer { struct Inner { int x; } m; };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 1u);
}

// ── c40 D-CSUBSET-POINTER-SUBTRACTION — `p - q` is ptrdiff_t (C 6.5.6p9) ──
// p-q (same pointer type) yields a SIGNED integer (ptrdiff_t/I64) = the element
// count, NOT a pointer. The fix lets it pass as a numeric function ARGUMENT (the
// sqlite `fmt - bufpt` blocker, ~50x S0003). Red-on-disable: revert and the
// pointer-difference is typed Ptr<T> → S_TypeMismatch when passed as an arg.

// c40a — the bug: p-q passed as a numeric function arg (char*).
TEST(SemanticAnalyzerCSubset, PointerSubtractionAsCallArgIsClean) {
    auto model = analyzeShipped("c-subset", {
        "void g(long x){ (void)x; }\n"
        "int f(char* a, char* b){ g(a - b); return 0; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}

// c40b — int* p-q also passes as a numeric arg (the type is I64 regardless of pointee).
TEST(SemanticAnalyzerCSubset, PointerSubtractionIntPtrAsCallArgIsClean) {
    auto model = analyzeShipped("c-subset", {
        "void g(long x){ (void)x; }\n"
        "int f(int* a, int* b){ g(a - b); return 0; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}

// c40c — p-q returned as long (the result type is ptrdiff_t) + assigned.
TEST(SemanticAnalyzerCSubset, PointerSubtractionReturnAndAssignIsClean) {
    auto model = analyzeShipped("c-subset", {
        "long span(char* a, char* b){ long n = a - b; return n; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}

// c40d — GUARD: MISMATCHED-pointee `char* - int*` does NOT get the ptrdiff rule
// (same-pointee only) → it stays a Ptr-typed value. When that value is passed to
// a NUMERIC param the call-arg `isAssignable` rejects it (S_TypeMismatch). NOTE:
// this is the ARG-CONTEXT catch ONLY — in a non-arg context (`(int)(a-b)`,
// `long n=a-b`) a mismatched-pointee difference is NOT diagnosed today (a
// deferred general fail-loud, D-CSUBSET-POINTER-DIFF-EDGE-CASES, flagged by the
// c40 audit). This pin documents the arg-context behavior, not a universal flag.
TEST(SemanticAnalyzerCSubset, MismatchedPointeeSubtractionRejectedAsNumericArg) {
    auto model = analyzeShipped("c-subset", {
        "void g(long x){ (void)x; }\n"
        "int f(char* a, int* b){ g(a - b); return 0; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_GE(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u);
}

// ── c41 D-CSUBSET-POINTER-INT-ARITHMETIC — `p ± n` is a pointer (C 6.5.6p8) ──
// `p + n` / `n + p` / `p - n` yield a pointer (Ptr<T>), and the MIR scales n by
// sizeof(*p). These pins assert the TYPE (the runtime stride is the corpus
// pointer_int_arith). Red-on-disable for c41b: revert the semantic `n + p` arm
// and `n + p` types as Int -> assigning it to `int*` fails S_TypeMismatch.

TEST(SemanticAnalyzerCSubset, PointerPlusIntIsCleanPointerTyped) {
    auto model = analyzeShipped("c-subset", {
        "int f(int* p, int n){ int* q = p + n; return *q; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}
// The key commutative case: `n + p` must type as a pointer, not the integer.
TEST(SemanticAnalyzerCSubset, IntPlusPointerIsCleanPointerTyped) {
    auto model = analyzeShipped("c-subset", {
        "int f(int* p, int n){ int* q = n + p; return *q; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}
TEST(SemanticAnalyzerCSubset, PointerMinusIntIsCleanPointerTyped) {
    auto model = analyzeShipped("c-subset", {
        "int f(int* p, int n){ int* q = p - n; return *q; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}
// GUARD: `int + int` is NOT pointer arithmetic (the Ptr guard is not over-broad).
TEST(SemanticAnalyzerCSubset, IntPlusIntUnaffectedByPtrArith) {
    auto model = analyzeShipped("c-subset", {
        "int f(int a, int b){ return a + b; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
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

// ── FC17.9(d) cycle 1b: `_Atomic` type qualifier (D-CSUBSET-ATOMIC) ──────────
//
// PHASE A pin: `_Atomic int x;` must PARSE (no P0001) and resolve to a type whose
// raw qualifier mask carries the Atomic bit — the front-end acceptance gate. The
// skin is TRANSPARENT, so `kind()` still sees the material I32; only the RAW
// `isAtomicQualified` query observes the qualifier. Volatile is NOT set (the atomic
// scan must not over-fire).
TEST(SemanticAnalyzerCSubset, AtomicQualifierResolvesAtomicQualified) {
    auto cu = buildShippedUnit("c-subset", {
        "_Atomic int x;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* x = findSym(model, "x");
    ASSERT_NE(x, nullptr);
    ASSERT_TRUE(x->type.valid());
    EXPECT_TRUE(ti.isAtomicQualified(x->type))
        << "_Atomic int resolves to atomicQualified(int)";
    EXPECT_FALSE(ti.isVolatileQualified(x->type))
        << "a plain _Atomic must NOT set the Volatile bit";
    EXPECT_EQ(ti.kind(x->type), TypeKind::I32)
        << "the qualifier skin is transparent — the material type is bare int";
}

// D-CSUBSET-ATOMIC-NONLOCKFREE (code-audit CRITICAL C1): `_Atomic` on an AGGREGATE
// (struct/union/by-value array) must FAIL LOUD at type resolution. The qualifier is a
// TRANSPARENT skin, so a wrapped aggregate would reach codegen, `computeLayout` strips
// the skin, and the copy decomposes to plain field Load/Store the type-based belt cannot
// see — a SILENT non-atomic access. RED-ON-DISABLE: remove the `isByValueClass` reject at
// the base-position `_Atomic` wrap and this count drops to 0 (the aggregate is silently
// atomic-wrapped, then non-atomically copied).
TEST(SemanticAnalyzerCSubset, AtomicOnAggregateFailsLoudNonLockFree) {
    auto cu = buildShippedUnit("c-subset", {
        "struct S { int a; int b; };\n"
        "_Atomic struct S x;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AtomicNonLockFree), 1u)
        << "_Atomic on an aggregate must fail loud (non-lock-free — deferred)";
}

// The NEGATIVE that keeps the gate honest: a lock-free SCALAR `_Atomic` must NOT be
// rejected (it is the supported case — it resolves to atomicQualified + lowers to the
// atomic opcodes). Without it, a too-broad reject would silently break scalar atomics.
TEST(SemanticAnalyzerCSubset, AtomicOnScalarNotRejectedNonLockFree) {
    auto cu = buildShippedUnit("c-subset", {
        "_Atomic int x;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AtomicNonLockFree), 0u)
        << "a lock-free scalar _Atomic must be accepted (the supported case)";
}

// The user-named combination: `_Atomic volatile int` must set BOTH bits in the ONE
// shared skin (cycle 1a's `qualified` merges {V}+{A}). Order-independent: the
// reverse spelling `volatile _Atomic int` resolves to the SAME interned type.
TEST(SemanticAnalyzerCSubset, AtomicVolatileSetsBothQualifierBits) {
    auto cu = buildShippedUnit("c-subset", {
        "_Atomic volatile int a;\n"
        "volatile _Atomic int b;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* a = findSym(model, "a");
    ASSERT_NE(a, nullptr);
    ASSERT_TRUE(a->type.valid());
    EXPECT_TRUE(ti.isAtomicQualified(a->type))
        << "_Atomic volatile int carries the Atomic bit";
    EXPECT_TRUE(ti.isVolatileQualified(a->type))
        << "_Atomic volatile int ALSO carries the Volatile bit (one {V,A} skin)";
    EXPECT_EQ(ti.kind(a->type), TypeKind::I32)
        << "transparent skin — material int";
    SymbolRecord const* b = findSym(model, "b");
    ASSERT_NE(b, nullptr);
    ASSERT_TRUE(b->type.valid());
    EXPECT_EQ(a->type, b->type)
        << "`_Atomic volatile int` and `volatile _Atomic int` intern to the SAME "
           "type — the bitset merge is order-independent";
}

// Red-on-disable guard for bit INDEPENDENCE: plain `volatile int` must carry ONLY
// the Volatile bit, never Atomic — proves the new atomic scan/wrap is a DISTINCT
// bit and does not leak onto volatile. (Delete the atomic wrap and this stays
// green; but paired with AtomicQualifierResolvesAtomicQualified it pins that the
// two qualifiers are separate — a volatile-tags-atomic regression fails here.)
TEST(SemanticAnalyzerCSubset, PlainVolatileIsNotAtomicQualified) {
    auto cu = buildShippedUnit("c-subset", {
        "volatile int v;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->type.valid());
    EXPECT_TRUE(ti.isVolatileQualified(v->type));
    EXPECT_FALSE(ti.isAtomicQualified(v->type))
        << "volatile alone must NOT carry the Atomic bit";
}

// Both resolver arms: a WEST `_Atomic` qualifies the POINTEE (`_Atomic int *p` =>
// Ptr<atomicQualified(int)>, pointer NOT atomic); an EAST `_Atomic` qualifies the
// POINTER OBJECT (`int * _Atomic q` => atomicQualified(Ptr<int>), pointee NOT
// atomic). This exercises resolveTypeNodeImpl's base arm AND declaratorDeclaredType's
// pointer-layer arm — the volatile-pointee/pointer-object mirror.
TEST(SemanticAnalyzerCSubset, AtomicPointeeVsPointerObject) {
    auto cu = buildShippedUnit("c-subset", {
        "_Atomic int *p;\n"
        "int * _Atomic q;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* p = findSym(model, "p");
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(p->type.valid());
    ASSERT_EQ(ti.kind(p->type), TypeKind::Ptr);
    EXPECT_FALSE(ti.isAtomicQualified(p->type))
        << "`_Atomic int *p` — the POINTER object is not atomic";
    EXPECT_TRUE(ti.isAtomicQualified(ti.operands(p->type)[0]))
        << "`_Atomic int *p` — the POINTEE is atomic (Ptr<atomicQualified(int)>)";
    SymbolRecord const* q = findSym(model, "q");
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(q->type.valid());
    ASSERT_EQ(ti.kind(q->type), TypeKind::Ptr);
    EXPECT_TRUE(ti.isAtomicQualified(q->type))
        << "`int * _Atomic q` — the POINTER object is atomic (atomicQualified(Ptr<int>))";
    EXPECT_FALSE(ti.isAtomicQualified(ti.operands(q->type)[0]))
        << "`int * _Atomic q` — the POINTEE is not atomic";
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
    // main + a + b + the 2 FC12a-core builtin TYPES (__va_list_tag + va_list) + the
    // 5 intrinsic builtins (c103 __umulh + c104 _InterlockedCompareExchange + c113
    // _ReadWriteBarrier + c115 _exception_code + _exception_info) + the 6 FC17.9(b)
    // bit-count builtins (__builtin_{popcount,clz,ctz}{,ll},
    // D-CSUBSET-BITCOUNT-INTRINSICS) + the 56 FC17.9(b) <stdbit.h>
    // __builtin_stdc_<op>_<T> intrinsics (D-FULLC-STDBIT) + the 2 FC17.9(d) atomic
    // accessors (atomic_load_explicit + atomic_store_explicit, D-CSUBSET-ATOMIC) +
    // the 2 FC17.5 predefined function-name symbols (__func__ + __FUNCTION__) — the
    // multiplication must mint NO symbol.
    EXPECT_EQ(model.symbols().size() - 1, 80u)
        << "main + a + b + __va_list_tag + va_list + the 5 intrinsic builtins + "
           "the 6 __builtin bit-count intrinsics + the 56 __builtin_stdc_* "
           "<stdbit.h> intrinsics + atomic_load_explicit + atomic_store_explicit + "
           "the 4 __builtin_complex/creal/cimag/conj complex builtins + "
           "__func__ + __FUNCTION__ — the multiplication mints none";
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

// FC16 (D-CSUBSET-NORETURN): the surviving Function symbol named `name` is
// noreturn (its `isNoreturn` bit). Mirrors `countSurvivingFns` — the `!isAbsorbedProto`
// filter isolates the single callable record a call resolves to.
[[nodiscard]] inline bool
survivingFnIsNoreturn(SemanticModel const& model, std::string_view name) {
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        auto const& r = model.symbols()[i];
        if (r.name == name && r.kind == DeclarationKind::Function
            && !r.isAbsorbedProto) {
            return r.isNoreturn;
        }
    }
    return false;
}

// (f) FC16 (D-CSUBSET-NORETURN): a prototype that spells `_Noreturn` + a
// definition that does NOT must OR-merge the noreturn attribute INTO the surviving
// record (the definition — the proto is absorbed). A call resolves to the survivor,
// so without the merge the call site would not see the attribute. Witnesses the
// post-1.5 mergedFnDecls OR-merge. RED-ON-DISABLE: drop the OR-merge → detection
// only marked the absorbed proto, so the survivor's isNoreturn stays false and the
// EXPECT_TRUE flips.
TEST(SemanticAnalyzerCSubset, NoreturnProtoMergesIntoDefinition) {
    auto model = analyzeShipped("c-subset", {
        "_Noreturn void die(int);\n"
        "void die(int x){ while(1){} }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a _Noreturn proto + a compatible definition must merge cleanly";
    EXPECT_EQ(countSurvivingFns(model, "die"), 1u);
    EXPECT_TRUE(survivingFnIsNoreturn(model, "die"))
        << "the _Noreturn on the proto must OR-merge into the surviving definition";
}

// (g) FC16 (D-CSUBSET-NORETURN): a shipped-descriptor symbol declared
// `"noreturn": true` (the abort/exit shape) threads onto the injected
// SymbolRecord's isNoreturn — a shipped extern has no user prototype to carry
// `_Noreturn`. Witnesses ShippedSymbol.noreturn -> SymbolRecord.isNoreturn at the
// injection site; a sibling symbol without the key stays non-noreturn.
// RED-ON-DISABLE: drop `rec.isNoreturn = sym.noreturn` at injection -> `boom`
// stays false.
TEST(SemanticAnalyzerCSubset, NoreturnShippedDescriptorSymbolIsNoreturn) {
    dss::test_support::ScratchDir sysDir{
        dss::test_support::Location::Temp, "nr-desc"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "boom.json",
        R"({ "header": "boom.h", "library": { "pe": "msvcrt.dll", "elf": "libc.so.6" },
             "symbols": [ { "name": "boom",  "signature": "fn() -> void", "noreturn": true },
                          { "name": "plain", "signature": "fn() -> void" } ] })",
        "#include <boom.h>\nint main() { return 0; }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    ASSERT_FALSE(model.hasErrors());
    bool sawBoom = false, sawPlain = false, boomNr = false, plainNr = false;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        auto const& r = model.symbols()[i];
        if (r.name == "boom")  { sawBoom = true;  boomNr  = r.isNoreturn; }
        if (r.name == "plain") { sawPlain = true; plainNr = r.isNoreturn; }
    }
    ASSERT_TRUE(sawBoom);
    ASSERT_TRUE(sawPlain);
    EXPECT_TRUE(boomNr)
        << "a descriptor `noreturn:true` symbol must inject SymbolRecord.isNoreturn";
    EXPECT_FALSE(plainNr)
        << "a descriptor symbol without `noreturn` stays non-noreturn";
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
// FC16 D-CSUBSET-ANON-MEMBER-PROMOTION (C11/C23 §6.7.2.1 ¶13): the members of
// an anonymous struct/union member are PROMOTED into the enclosing composite's
// member namespace. `struct S { union { int a; int b; }; } s; s.a` resolves `a`
// as if a direct member. Pins: promotion resolves clean (no S0017), member
// access types through the anon composite, a DIRECT-member collision fails
// loud, and an AMBIGUOUS sibling-anon name fails loud.
// ─────────────────────────────────────────────────────────────────────────

// (a) The exact S0017 probe from the feature request now resolves clean: an
// anonymous union member whose fields are read as direct members of S.
TEST(SemanticAnalyzerCSubset, AnonUnionMemberPromotesAndResolves) {
    auto model = analyzeShipped("c-subset", {
        "struct S { union { int a; int b; }; };\n"
        "int main() { struct S s; s.a = 42; return s.a; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "s.a / s.b promoted from the anonymous union must resolve clean";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeclarationDeclaresNothing), 0u)
        << "an anonymous COMPOSITE member is not a declares-nothing form";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "s.a must resolve through the anonymous union, not be undeclared";
    // The promoted field `a` carries an anonAncestorPath (reached via the anon
    // union member) and types as I32.
    SymbolRecord const* a = fieldSym(model, "a");
    ASSERT_NE(a, nullptr);
    EXPECT_FALSE(a->anonAncestorPath.empty())
        << "a is reachable only through the anonymous union member";
    ASSERT_TRUE(a->type.valid());
    EXPECT_EQ(model.lattice().interner().kind(a->type), TypeKind::I32);
}

// (b) A NAMED direct member alongside an anonymous union: both resolve, and the
// anon member itself is flagged isAnonymousMember.
TEST(SemanticAnalyzerCSubset, AnonUnionMemberBesideNamedMember) {
    auto model = analyzeShipped("c-subset", {
        "struct S { int tag; union { int i; int j; }; };\n"
        "int main() { struct S s; s.tag = 1; s.i = 41; return s.j; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    // The synthetic anon field is present and flagged.
    bool sawAnon = false;
    for (std::size_t k = 1; k < model.symbols().size(); ++k)
        if (model.symbols()[k].isAnonymousMember) sawAnon = true;
    EXPECT_TRUE(sawAnon) << "the anon union member must be flagged isAnonymousMember";
}

// (c) A nested anonymous struct inside an anonymous union — two-level promotion.
TEST(SemanticAnalyzerCSubset, NestedAnonMemberPromotes) {
    auto model = analyzeShipped("c-subset", {
        "struct S { union { struct { int x; int y; }; int packed; }; };\n"
        "int main() { struct S s; s.x = 40; s.y = 2; return s.x + s.y; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "x/y promoted through anon-struct-in-anon-union must resolve clean";
    SymbolRecord const* x = fieldSym(model, "x");
    ASSERT_NE(x, nullptr);
    // Reached through TWO anonymous members (union then struct).
    EXPECT_EQ(x->anonAncestorPath.size(), 2u)
        << "x is two anonymous levels deep";
}

// (d) A promoted name colliding with a DIRECT member fails loud (C 6.7.2.1 ¶13).
TEST(SemanticAnalyzerCSubset, AnonMemberCollisionWithDirectFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "struct S { int x; union { int x; int y; }; };\n",
    });
    EXPECT_TRUE(model.hasErrors());
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "a promoted member `x` colliding with the direct member `x` must fail loud";
}

// (d2) The collision check must be scoped to the enclosing COMPOSITE's own
// field members — NOT the parent scope chain. C 6.2.1 gives each struct/union a
// SEPARATE member name space disjoint from ordinary identifiers, so a promoted
// member sharing a name with an outer GLOBAL / TYPEDEF / function is LEGAL and
// must NOT false-error. Regression guard for the parent-walk `lookup` bug.
TEST(SemanticAnalyzerCSubset, AnonMemberNameMayShadowOuterIdentifier) {
    auto globalModel = analyzeShipped("c-subset", {
        "int a;\n"
        "struct S { struct { int a; int b; }; };\n"
        "int main() { struct S s; s.a = 40; s.b = 2; return s.a + s.b; }\n",
    });
    EXPECT_FALSE(globalModel.hasErrors())
        << "a promoted member `a` sharing a name with a global `a` is legal (C 6.2.1)";
    EXPECT_EQ(countCode(globalModel.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "no false S_RedeclaredSymbol against an enclosing-scope identifier";
    // A typedef in the enclosing scope must likewise not false-collide.
    auto typedefModel = analyzeShipped("c-subset", {
        "typedef int a;\n"
        "struct S { struct { int a; int b; }; };\n"
        "int main() { struct S s; s.a = 1; return 0; }\n",
    });
    EXPECT_EQ(countCode(typedefModel.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "no false S_RedeclaredSymbol against an enclosing typedef";
}

// (e) An AMBIGUOUS name shared by two sibling anonymous members fails loud on
// ACCESS (the promotion itself is fine — the ambiguity is a use-site error).
TEST(SemanticAnalyzerCSubset, AnonMemberAmbiguousSiblingFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "struct S { union { int a; }; union { int a; }; };\n"
        "int main() { struct S s; return s.a; }\n",
    });
    EXPECT_TRUE(model.hasErrors());
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "s.a matching two sibling anonymous unions is ambiguous — fail loud";
}

// (f) A BIT-FIELD inside an anonymous composite must NOT trigger a false
// S_BitFieldNonIntegerType. Regression guard: the Pass-1.5 anon-composite arm
// must NOT run resolveBitfieldSuffix on the composite field node (its bounded
// descendant search would find the INNER `: W` suffix and validate that width
// against the composite HEAD type — a non-integer — falsely). The inner
// bit-field is resolved by the anon composite's own visit; promotion resolves
// its members clean.
TEST(SemanticAnalyzerCSubset, AnonUnionWithInnerBitfieldNoFalseError) {
    auto model = analyzeShipped("c-subset", {
        "struct S { union { int a : 4; int b; }; };\n"
        "int main() { struct S s; s.b = 42; return s.a; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a bit-field inside an anonymous union must not false-error";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_BitFieldNonIntegerType), 0u)
        << "no false S_BitFieldNonIntegerType from the anon-composite arm";
    // Both members promote and resolve; the bit-field width is recorded on `a`.
    SymbolRecord const* a = fieldSym(model, "a");
    ASSERT_NE(a, nullptr);
    EXPECT_FALSE(a->anonAncestorPath.empty());
    ASSERT_TRUE(a->bitFieldWidth.has_value())
        << "the inner bit-field width is still resolved by the union's own visit";
    EXPECT_EQ(*a->bitFieldWidth, 4u);
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

// ── c99 (D-CSUBSET-FAM-IN-UNION-MEMBER) ──────────────────────────────────────
// C99 §6.7.2.1p18 forbids a flexible-array-member-bearing struct as a member of a
// STRUCTURE or an ELEMENT OF AN ARRAY — it says nothing about a UNION, and
// gcc/clang both accept a FAM-struct as a DIRECT union member (sqlite's
// `union { SrcList sSrc; u8 srcSpace[N]; }` stack-slab idiom). So a direct
// FAM-struct union member is PERMITTED (no S_FlexibleArrayInAggregate); a
// FAM-struct as a struct member stays fail-loud AT the carve-out branch, and an
// array-of-FAM-struct as a union member (the p18 "element of an array" case) stays
// fail-loud UPSTREAM at array construction (applyArraySuffix → InvalidType), never
// reaching the union carve-out. The two genuine red-on-disable change-guards for
// the `ck==Union` gate are the accepted/struct-rejected pins; the array and
// union-of-union pins lock the enforcement boundary on either side.

// PERMITTED: a FAM-struct as a DIRECT union member — no S001D. (The sqlite blocker.)
TEST(SemanticAnalyzerCSubset, FlexibleArrayStructAsUnionMemberIsAccepted) {
    auto model = analyzeShipped("c-subset", {
        "struct Slab { int n; int a[]; };\n"
        "union U { struct Slab s; char space[16]; };\n"
        "int main(void){ union U u; return (int)sizeof(u); }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_FlexibleArrayInAggregate), 0u)
        << "a FAM-bearing struct is a legal DIRECT union member (gcc/clang accept it)";
    // The union must still size (layout not rejected): sizeof(U)==16 (max of the
    // 4-byte Slab prefix and the 16-byte space[]) — pinned end-to-end in the
    // fam_struct_in_union_member example + TypeLayout.UnionWith… unit test.
    EXPECT_FALSE(model.hasErrors())
        << "the whole TU is well-typed once the union FAM member is permitted";
}

// STILL FORBIDDEN: a FAM-struct as a STRUCT member → S001D (C99 p18, unchanged
// DSS posture). This is a GENUINE red-on-disable change-guard: the rejection is
// emitted at the carve-out branch itself (ck==Struct ⇒ `permittedAsUnionMember`
// false ⇒ famDiag). Widen the `ck==Union` gate to permit a FAM-struct in ANY
// composite and this struct-member case would wrongly pass.
TEST(SemanticAnalyzerCSubset, FlexibleArrayStructAsStructMemberStillRejected) {
    auto model = analyzeShipped("c-subset", {
        "struct Slab { int n; int a[]; };\n"
        "struct Bad { struct Slab s; int x; };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_FlexibleArrayInAggregate), 1u)
        << "a FAM-bearing struct as a STRUCT member is C99 p18 ill-formed — must fail loud";
}

// STILL FORBIDDEN: an ARRAY of a FAM-struct as a UNION member → S001D. p18's
// "element of an array" bans this even inside a union. The enforcement is UPSTREAM
// of the c99 carve-out: an array whose element embeds a FAM is rejected at array
// construction (semantic_analyzer.cpp applyArraySuffix, ~1630/1972 → InvalidType),
// so this field's type is already invalid before the union carve-out runs — it
// never reaches that branch. This is therefore a POSTURE regression-guard
// (array-of-FAM stays rejected regardless of the union relaxation), NOT a
// red-on-disable guard for the `ck==Union` gate — widening/removing that gate does
// not affect this case (verified by the c99 audit). Kept as defense-in-depth.
TEST(SemanticAnalyzerCSubset, ArrayOfFlexibleArrayStructInUnionStillRejected) {
    auto model = analyzeShipped("c-subset", {
        "struct Slab { int n; int a[]; };\n"
        "union Bad { struct Slab arr[3]; char space[64]; };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_FlexibleArrayInAggregate), 1u)
        << "an ARRAY of a FAM-struct is a p18 'element of an array' violation even in a union";
}

// PERMITTED (gcc-parity, locks the completeness boundary): a UNION whose member is
// itself a union that (transitively) contains a FAM-struct is ALSO accepted. p18
// restricts only struct-membership and array-elementhood; a union member of a union
// is p18-legal, and `typeContainsFlexibleArray` does not recurse into unions, so the
// inner FAM never reaches the carve-out. gcc/clang accept it (c99 audit verified,
// S001D=0). This is a POSTURE/parity guard (not a red-on-disable for the `ck==Union`
// gate): it pins that the simplified gate does NOT over-reject the nested-union form
// — the exact case the `kind(ft)==Struct` tautology, had it been kept, would have
// wrongly rejected under a future recursion change.
TEST(SemanticAnalyzerCSubset, UnionContainingFamStructAsUnionMemberIsAccepted) {
    auto model = analyzeShipped("c-subset", {
        "struct Slab { int n; int a[]; };\n"
        "union Inner { struct Slab s; int x; };\n"
        "union Outer { union Inner v; char space[8]; };\n"
        "int main(void){ union Outer o; return (int)sizeof(o); }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_FlexibleArrayInAggregate), 0u)
        << "a union member that is a union containing a FAM-struct is p18-legal (gcc/clang accept)";
    EXPECT_FALSE(model.hasErrors())
        << "the nested-union form is well-typed — the carve-out must not over-reject it";
}

// ── C23 nullptr (D-CSUBSET-NULLPTR) ───────────────────────────────────────────
// `void *p = nullptr;` + `p == nullptr` / `p != nullptr` are admitted — nullptr is a
// null pointer constant assignable to, and comparable with, any pointer. No error.
TEST(SemanticAnalyzerCSubset, NullptrInitsAndComparesPointer) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(void){ void *p = nullptr; int r = p == nullptr; p = nullptr;"
        " return r + (nullptr == p); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// `int x = nullptr;` is a constraint violation — nullptr converts only to pointers.
TEST(SemanticAnalyzerCSubset, NullptrToIntFailsLoud) {
    auto cu = buildShippedUnit("c-subset", { "int f(void){ int x = nullptr; return x; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u);
}

// `bool b = nullptr;` is rejected — nullptr→bool is DEFERRED (the c-subset has no
// scalar→bool conversion; D-CSUBSET-NULLPTR-BOOL-CONVERSION), so it stays consistent
// with `bool b = 0;` (also a mismatch) rather than admit-then-fail-at-codegen.
TEST(SemanticAnalyzerCSubset, NullptrToBoolFailsLoud) {
    auto cu = buildShippedUnit("c-subset", { "int f(void){ bool b = nullptr; return 0; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u);
}

// The fail-loud operator gate: nullptr in arithmetic (`nullptr + 1`) is rejected —
// WITHOUT the gate the HIR lowering (nullptr → integer 0) would silently accept it.
TEST(SemanticAnalyzerCSubset, NullptrArithmeticFailsLoud) {
    auto cu = buildShippedUnit("c-subset", { "void *f(void){ return nullptr + 1; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 1u);
}

// The gate rejects a RELATIONAL comparison (`nullptr < p`) — only `==`/`!=` against a
// pointer/nullptr peer is admissible.
TEST(SemanticAnalyzerCSubset, NullptrRelationalFailsLoud) {
    auto cu = buildShippedUnit("c-subset", { "int f(void *p){ return nullptr < p; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 1u);
}

// `==` against a NON-pointer, NON-nullptr peer (`nullptr == 5`) is rejected.
TEST(SemanticAnalyzerCSubset, NullptrEqualsIntFailsLoud) {
    auto cu = buildShippedUnit("c-subset", { "int f(void){ return nullptr == 5; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 1u);
}

// The gate must NOT fire on a plain assignment `p = nullptr` (handled by isAssignable)
// — the false-positive that the Assign/Comma/compound classification fix closed.
TEST(SemanticAnalyzerCSubset, NullptrPlainAssignNoFalsePositive) {
    auto cu = buildShippedUnit("c-subset", {
        "int f(void *p){ p = nullptr; return p == nullptr; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// A COMPOUND assignment (`p += nullptr`) IS pointer arithmetic → rejected.
TEST(SemanticAnalyzerCSubset, NullptrCompoundAssignFailsLoud) {
    auto cu = buildShippedUnit("c-subset", { "int f(void *p){ p += nullptr; return 0; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 1u);
}

// unary `-nullptr` is rejected (Neg on nullptr is not a valid operand).
TEST(SemanticAnalyzerCSubset, NullptrUnaryNegFailsLoud) {
    auto cu = buildShippedUnit("c-subset", { "void *f(void){ return -nullptr; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 1u);
}

// `nullptr` passed as a VARIADIC argument is rejected (no default arg promotion).
TEST(SemanticAnalyzerCSubset, NullptrVariadicArgFailsLoud) {
    auto cu = buildShippedUnit("c-subset", {
        "int g(int n, ...);\n"
        "int f(void){ return g(1, nullptr); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 1u);
}

// A conditional with a nullptr arm (`c ? nullptr : p`, nullptr FIRST) types as the
// pointer — the combineTernary NullptrT arm; without it the ternary would mistype as
// NullptrT and a `void*` return would then mismatch.
TEST(SemanticAnalyzerCSubset, NullptrTernaryTypesAsPointer) {
    auto cu = buildShippedUnit("c-subset", {
        "void *f(int c, void *p){ return c ? nullptr : p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// A bare function designator / array name DECAYS to a pointer, so `nullptr == func`
// and `nullptr == arr` are valid C23 comparisons — the gate must NOT reject them.
// Regression pin for the Eq/Ne peer-decay fix (FnSig / Array peers, not just Ptr).
TEST(SemanticAnalyzerCSubset, NullptrComparedToDesignatorsAdmitted) {
    auto cu = buildShippedUnit("c-subset", {
        "int g(void);\n"
        "int f(void){ int a[4]; if (nullptr == g) return 1;"
        " if (nullptr == a) return 2; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 0u);
}

// sizeof(nullptr) folds to the pointer width (C23: sizeof(nullptr_t) == sizeof(void*))
// via the scalarByteSize NullptrT arm — a regression dropping the arm makes the fold
// fail (nullopt → error). Pins that it stays well-typed (size_t / U64).
TEST(SemanticAnalyzerCSubset, NullptrSizeofIsWellTyped) {
    auto cu = buildShippedUnit("c-subset", {
        "unsigned long f(void){ return sizeof(nullptr); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors());
}

// ─────────────────────────────────────────────────────────────────────────
// FC17 (D-CSUBSET-ENUM-UNDERLYING-TYPE, C23 6.7.2.2): the explicit enum
// underlying-type clause `enum E : T { … }`, riding the speculative-optional
// parser capability (D-PARSE-SPECULATIVE-OPTIONAL). The `enum <tag> :` prefix
// collides with the pre-existing anonymous enum-typed struct bit-field
// `enum Color : 4;`; the clause is TRIED after the `:` and ROLLS BACK to the
// bit-field reading when a type does not follow.
// ─────────────────────────────────────────────────────────────────────────

TEST(SemanticAnalyzerCSubset, EnumExplicitUnderlyingTypeSetsScalars) {
    auto cu = buildShippedUnit("c-subset", {
        "enum E : unsigned char { A, B };\n"
        "int main(void) { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "an explicit unsigned-char underlying enum must analyze clean";
    auto const& ti = model.lattice().interner();
    // The enumerator A carries the enum TypeId (erec.type = compositeTy).
    TypeId enumTy{};
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "A") enumTy = model.symbols()[i].type;
    ASSERT_TRUE(enumTy.valid()) << "enumerator A must be typed as the enum";
    EXPECT_EQ(ti.kind(enumTy), TypeKind::Enum);
    ASSERT_EQ(ti.scalars(enumTy).size(), 1u);
    // RED-on-disable: revert the grammar (parse fails, no clean enum) OR the
    // semantic threading (falls back to default I32) and this drops to I32.
    EXPECT_EQ(ti.scalars(enumTy)[0], static_cast<std::int64_t>(TypeKind::U8))
        << "the enum underlying scalar must be U8, not the default I32";
}

TEST(SemanticAnalyzerCSubset, EnumUnderlyingTypeDistinctFromDefault) {
    auto cu = buildShippedUnit("c-subset", {
        "enum Wide { WA };\n"                     // default int
        "enum Narrow : unsigned char { NA };\n"   // explicit u8
        "int main(void) { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors());
    auto const& ti = model.lattice().interner();
    TypeId wide{}, narrow{};
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        auto const& n = model.symbols()[i].name;
        if (n == "WA") wide   = model.symbols()[i].type;
        if (n == "NA") narrow = model.symbols()[i].type;
    }
    ASSERT_TRUE(wide.valid() && narrow.valid());
    EXPECT_NE(wide.v, narrow.v)
        << "a different underlying type interns a DISTINCT enum TypeId";
    EXPECT_EQ(ti.scalars(wide)[0],   static_cast<std::int64_t>(TypeKind::I32));
    EXPECT_EQ(ti.scalars(narrow)[0], static_cast<std::int64_t>(TypeKind::U8));
}

TEST(SemanticAnalyzerCSubset, EnumAnonymousWithExplicitUnderlying) {
    auto cu = buildShippedUnit("c-subset", {
        "enum : unsigned char { AX, AY };\n"      // anonymous + explicit underlying
        "int main(void) { return AY; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors())
        << "an anonymous enum with an explicit underlying type must be clean";
    auto const& ti = model.lattice().interner();
    TypeId enumTy{};
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "AX") enumTy = model.symbols()[i].type;
    ASSERT_TRUE(enumTy.valid());
    EXPECT_EQ(ti.scalars(enumTy)[0], static_cast<std::int64_t>(TypeKind::U8));
}

TEST(SemanticAnalyzerCSubset, EnumUnderlyingNonIntegerFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "enum E : float { A };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidEnumUnderlyingType), 1u)
        << "a float underlying type must fail loud S_InvalidEnumUnderlyingType";
}

TEST(SemanticAnalyzerCSubset, EnumUnderlyingStructFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "struct Foo { int x; };\n"
        "enum E : struct Foo { A };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidEnumUnderlyingType), 1u)
        << "a struct underlying type must fail loud S_InvalidEnumUnderlyingType";
}

TEST(SemanticAnalyzerCSubset, EnumeratorValueOutOfRangeFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "enum E : unsigned char { A = 256 };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_EnumeratorValueOutOfRange), 1u)
        << "256 does not fit unsigned char (max 255) — must fail loud";
}

TEST(SemanticAnalyzerCSubset, EnumeratorValueAtBoundaryClean) {
    auto model = analyzeShipped("c-subset", {
        "enum E : unsigned char { A = 255 };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_EnumeratorValueOutOfRange), 0u)
        << "255 is exactly the unsigned-char max — in range, no error";
    EXPECT_FALSE(model.hasErrors());
}

TEST(SemanticAnalyzerCSubset, EnumDefaultUnderlyingRangeCheckNeverFires) {
    // A default-int enum is NEVER range-checked (hasExplicitUnderlying == false),
    // so a value that would overflow a narrow type but fits int is clean — the
    // C-classic behavior is unchanged.
    auto model = analyzeShipped("c-subset", {
        "enum E { A = 256, B = 70000 };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_EnumeratorValueOutOfRange), 0u)
        << "a default-int enum range check must NEVER fire";
    EXPECT_FALSE(model.hasErrors());
}

TEST(SemanticAnalyzerCSubset, EnumBitfieldWidthValidatesAgainstExplicitUnderlying) {
    // A bit-field of an enum with an EXPLICIT unsigned-char underlying validates
    // its width against 8 bits (the existing bit-field check reads the enum
    // underlying via enumUnderlyingOrSelf, scalars[0] = U8): width 9 exceeds it
    // and fails loud; width 8 is exactly in range.
    auto bad = analyzeShipped("c-subset", {
        "enum E : unsigned char { A };\n"
        "struct S { enum E f : 9; };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(bad.diagnostics(),
                        DiagnosticCode::S_BitFieldWidthOutOfRange), 1u)
        << "width 9 > the U8 underlying 8 bits must fail loud";
    auto ok = analyzeShipped("c-subset", {
        "enum E : unsigned char { A };\n"
        "struct S { enum E f : 8; };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(ok.diagnostics(),
                        DiagnosticCode::S_BitFieldWidthOutOfRange), 0u)
        << "width 8 == the U8 underlying 8 bits is exactly in range";
}

TEST(SemanticAnalyzerCSubset, EnumAnonymousBitfieldColonSurvivesSpeculativeOptional) {
    // THE FORK PIN (Option B): the pre-existing anonymous enum-typed struct
    // bit-field `enum Color : 3;` must STILL parse after adding the speculative
    // underlying-type clause — the speculative optional TRIES `: <type>` and
    // ROLLS BACK to the bit-field reading when an int-constant (not a type)
    // follows the colon. A plain non-speculative optional (Option A) would break
    // this with a loud parse error.
    auto model = analyzeShipped("c-subset", {
        "enum Color { RED, GREEN };\n"
        "struct S { int a; enum Color : 3; int b; };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "`enum Color : 3;` (anonymous bit-field) must survive the speculative "
           "underlying-type optional";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::P_NoAlternativeMatched), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidEnumUnderlyingType), 0u)
        << "`: 3` must be a bit-field width, NOT a (failed) underlying type";
}

TEST(SemanticAnalyzerCSubset, EnumUnderlyingBareIdentifierWidthRollsBackToBitfield) {
    // The requireKnownType triage pin: `enum Color : W3` where W3 is a VALUE
    // identifier (an enum constant, NOT a type) rolls the speculative
    // underlying-type clause back so `: W3` is the anonymous bit-field width.
    auto model = analyzeShipped("c-subset", {
        "enum Widths { W3 = 3 };\n"
        "enum Color { RED, GREEN };\n"
        "struct S { int a; enum Color : W3; int b; };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a value width identifier must roll back to the bit-field reading";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidEnumUnderlyingType), 0u)
        << "`: W3` (a value) must NOT be treated as an underlying type";
}

// ── FC17 (D-CSUBSET-CONSTEXPR): C23 6.7.1 `constexpr` OBJECT storage-class ──
//
// THE EMPIRICAL DELTA vs `const` (the feature's reason to exist): `const` is
// initializer-blind — `const int x = argc;` compiles clean and only an ICE
// consumer errors lazily — while `constexpr` must fail AT ITS OWN DECLARATION
// when the initializer is not a compile-time constant (6.7.1p10). The pair
// below pins BOTH sides so the delta can never silently collapse.
// RED-ON-DISABLE: bypass the Pass-2 validateConstexprDeclarator hook and the
// constexpr arm goes green-on-argc (0 diagnostics) → the EXPECT_EQ(…, 1u) reds.
TEST(SemanticAnalyzerCSubset, ConstexprDeltaVsConst) {
    auto constModel = analyzeShipped("c-subset", {
        "int main(int argc, char **argv) { const int x = argc; return x; }\n",
    });
    EXPECT_FALSE(constModel.hasErrors())
        << "`const int x = argc;` is legal C — const is initializer-blind";
    auto cxModel = analyzeShipped("c-subset", {
        "int main(int argc, char **argv) { constexpr int x = argc; return x; }\n",
    });
    EXPECT_EQ(countCode(cxModel.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 1u)
        << "`constexpr int x = argc;` must fail AT THE DECLARATION";
}

// A folding constexpr is accepted AND usable in constant-expression position:
// `constexpr int N = 5;` dimensions `int a[N]` exactly as a const would — plus
// the symbol carries BOTH Pass-1 marks (isConstexpr + the implied isConst).
// RED-ON-DISABLE: drop the `specifierPrefixHasConstexpr` minting and the flag
// EXPECTs red (and every enforcement test in this block stops firing).
TEST(SemanticAnalyzerCSubset, ConstexprFoldsAndMarksSymbol) {
    auto model = analyzeShipped("c-subset", {
        "constexpr int N = 5;\n"
        "int main(void) { int a[N]; return sizeof(a); }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    SymbolRecord const* nRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "N") nRec = &model.symbols()[i];
    }
    ASSERT_NE(nRec, nullptr);
    EXPECT_TRUE(nRec->isConstexpr) << "Pass 1 must mark the symbol constexpr";
    EXPECT_TRUE(nRec->isConst) << "constexpr implies const (6.7.1p10)";
    // The array dimensioned through the constexpr constant.
    SymbolRecord const* aRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "a") aRec = &model.symbols()[i];
    }
    ASSERT_NE(aRec, nullptr);
    ASSERT_TRUE(aRec->type.valid());
    auto const& ti = model.lattice().interner();
    ASSERT_EQ(ti.kind(aRec->type), TypeKind::Array);
    EXPECT_EQ(ti.scalars(aRec->type)[0], 5);
}

// F2 (the shared-evaluator char/bool leaf arms): a constexpr char / bool object
// folds its keyword/char-constant initializer. RED-ON-DISABLE: remove the
// evaluator's fixed-value / narrow-char arms and both go S0037-red.
TEST(SemanticAnalyzerCSubset, ConstexprCharAndBoolFold) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) { constexpr char c = 'a'; constexpr bool b = true; "
        "return c + b; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "'a' and true are integer constant expressions (C23 6.6)";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 0u);
}

// F2 flip — the pre-existing static_assert gaps the shared-evaluator leaf arms
// close: `_Static_assert('a'==97)` and `_Static_assert(true)` FAILED S0029 at
// HEAD (empirically confirmed pre-change) because the CST evaluator's leaf only
// decoded integer-set tokens. Both now fold. RED-ON-DISABLE: remove either leaf
// arm and its assert reds.
TEST(SemanticAnalyzerCSubset, StaticAssertCharLiteralConditionFolds) {
    auto model = analyzeShipped("c-subset", {
        "_Static_assert('a' == 97, \"char folds\");\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "'a' is an integer character constant (value 97) — must fold";
    EXPECT_FALSE(model.hasErrors());
}

TEST(SemanticAnalyzerCSubset, StaticAssertTrueKeywordConditionFolds) {
    auto model = analyzeShipped("c-subset", {
        "_Static_assert(true, \"true folds\");\n"
        "_Static_assert(!false, \"false folds\");\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "true/false are config-declared fixed-value keyword literals";
    EXPECT_FALSE(model.hasErrors());
}

// F3 no-leak walls: the float capability added FOR constexpr must NOT leak into
// integer-required consumers — a float in a static_assert condition / an array
// dimension stays non-constant. RED-ON-DISABLE: populate floatLiteralTokens (or
// flip allowFloat) in constIntExpr's context and both EXPECTs red.
TEST(SemanticAnalyzerCSubset, FloatDoesNotLeakIntoIntegerConstExprConsumers) {
    auto saModel = analyzeShipped("c-subset", {
        "_Static_assert(1.5 > 1.0, \"floats are not ICEs\");\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(saModel.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 1u)
        << "a float condition is NOT an integer constant expression (C 6.7.10)";
    // VLA C1a (D-CSUBSET-VLA): pinned at FILE scope — a float bound is not an
    // integer constant, so the file-scope array stays S_NonConstantArrayLength (it
    // is not a VLA; a VLA needs automatic storage). A block-scope float bound would
    // become a VLA (fails at the LIR C1b boundary); the no-leak intent holds here.
    auto dimModel = analyzeShipped("c-subset", {
        "int a[1.5 + 1.5];\n",
    });
    EXPECT_EQ(countCode(dimModel.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 1u)
        << "a float array dimension must stay S_NonConstantArrayLength";
}

// The fixed-value map excludes NullptrT rows BY CONSTRUCTION: `nullptr`
// (literalTypes value 0, core NullptrT) is a null pointer constant, NOT an
// integer constant expression — it must not fold in integer const-expr
// position. RED-ON-DISABLE: drop the integer-valued-core filter in
// fixedValueTokenMap and both EXPECTs red (nullptr would fold to 0).
TEST(SemanticAnalyzerCSubset, NullptrStaysNonFoldableInIntegerConstExpr) {
    // VLA C1a (D-CSUBSET-VLA): pinned at FILE scope — `nullptr` is not an integer
    // constant, so the file-scope array stays S_NonConstantArrayLength (not a VLA).
    auto dimModel = analyzeShipped("c-subset", {
        "int a[nullptr];\n",
    });
    EXPECT_EQ(countCode(dimModel.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 1u);
    auto saModel = analyzeShipped("c-subset", {
        "_Static_assert(nullptr, \"nullptr is not an ICE\");\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(saModel.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 1u);
}

// F4 — constexpr is the OBJECT storage-class: BOTH function forms fail loud.
// The DEFINITION form is the F1 hook-hoist witness: a function definition's
// declarator is a BARE declarator carrier (no init slot), which the loop's
// `rule != initDeclaratorRule` gate skips — a post-gate hook would silently
// accept it (and the file-scope linkage row would wrongly give it INTERNAL
// linkage). RED-ON-DISABLE: move the hook below the gate and the definition
// arm reds while the proto arm stays green.
TEST(SemanticAnalyzerCSubset, ConstexprFunctionFormsFailLoud) {
    auto protoModel = analyzeShipped("c-subset", {
        "constexpr int f(void);\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(protoModel.diagnostics(),
                        DiagnosticCode::S_ConstexprFunctionNotSupported), 1u)
        << "a constexpr function PROTOTYPE must fail loud";
    auto defModel = analyzeShipped("c-subset", {
        "constexpr int f(void) { return 1; }\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(defModel.diagnostics(),
                        DiagnosticCode::S_ConstexprFunctionNotSupported), 1u)
        << "a constexpr function DEFINITION must fail loud (the F1 hoist "
           "witness — its declarator is a bare carrier the init gate skips)";
}

// F4 — missing initializer fires PER DECLARATOR: `a` folds fine, `b` has no
// init slot at all (a bare declarator in the list).
TEST(SemanticAnalyzerCSubset, ConstexprMissingInitializerFiresPerDeclarator) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) { constexpr int a = 1, b; return a; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConstexprMissingInitializer), 1u)
        << "exactly one missing-init — on `b`, not `a`";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 0u)
        << "`a = 1` folds — no false non-constant on the initialized slot";
}

// F4 — `for (constexpr int i = 0; ...)` is VALID C23 (6.8.5p3 admits
// auto/register/constexpr in a for-init; forDecl reuses localDeclSpecifiers so
// it parses, and forDecl has NO linkageSpecifiers so linkageFrom's empty-map
// early-return keeps it linkage-silent).
TEST(SemanticAnalyzerCSubset, ConstexprInForInitAccepted) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) { for (constexpr int i = 0;;) { return i; } }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "C23 6.8.5p3 explicitly admits constexpr in a for-init declaration";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticStorageInForInit), 0u)
        << "the for-init static gate must NOT fire on constexpr";
}

// F4 — the volatile pair (C23 6.7.1p11): a volatile-qualified OBJECT type is
// rejected; a volatile POINTEE stays legal (the object is the pointer). The
// east-volatile form (`int * volatile p`) IS a volatile object — rejected.
TEST(SemanticAnalyzerCSubset, ConstexprVolatileObjectRejectedPointeeLegal) {
    auto badModel = analyzeShipped("c-subset", {
        "int main(void) { constexpr volatile int v = 1; return v; }\n",
    });
    EXPECT_EQ(countCode(badModel.diagnostics(),
                        DiagnosticCode::S_ConstexprInvalidQualifier), 1u)
        << "a volatile-qualified constexpr OBJECT violates 6.7.1p11";
    auto okModel = analyzeShipped("c-subset", {
        "int main(void) { constexpr volatile int *p = nullptr; "
        "return p == 0 ? 0 : 1; }\n",
    });
    EXPECT_EQ(countCode(okModel.diagnostics(),
                        DiagnosticCode::S_ConstexprInvalidQualifier), 0u)
        << "a volatile POINTEE is legal — the constexpr object is the pointer";
    EXPECT_FALSE(okModel.hasErrors());
    auto eastModel = analyzeShipped("c-subset", {
        "int main(void) { constexpr int * volatile p = nullptr; return 0; }\n",
    });
    EXPECT_EQ(countCode(eastModel.diagnostics(),
                        DiagnosticCode::S_ConstexprInvalidQualifier), 1u)
        << "east `int * volatile p` IS a volatile-qualified object — rejected";
}

// F5 — an ENUM-typed constexpr is admitted as arithmetic: the enumerator
// initializer folds through the shared resolveSymbolValue direct-value arm.
TEST(SemanticAnalyzerCSubset, ConstexprEnumTypedAdmitted) {
    auto model = analyzeShipped("c-subset", {
        "enum Color { RED = 3, GREEN };\n"
        "int main(void) { constexpr enum Color c = GREEN; return c; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "an enumerator is a compile-time constant — enum constexpr folds";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 0u);
}

// Float-capable folding (the constExprValue arm): a float constexpr with a
// folding arithmetic initializer validates, including combined with `static`.
// RED-ON-DISABLE: revert the floatLiteralTokens population in constExprValue
// and this reds S0037 (the leaf would refuse the float literal).
TEST(SemanticAnalyzerCSubset, ConstexprFloatFolds) {
    auto model = analyzeShipped("c-subset", {
        "static constexpr double PI2 = 3.5 * 2;\n"
        "int main(void) { return (int)PI2; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "3.5 * 2 folds under allowFloat — the float-capable constexpr arm";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 0u);
}

// Pointer arm: nullptr and the folded integer-0 forms are null pointer
// constants (accepted); an address-of initializer is not a compile-time
// constant here (fail loud). The `(T*)0` cast form is the named loud deferral
// D-CSUBSET-CONSTEXPR-POINTER-CAST-NULL (falls into the same fail-loud arm).
TEST(SemanticAnalyzerCSubset, ConstexprPointerNullFormsAcceptedAddressRejected) {
    auto okModel = analyzeShipped("c-subset", {
        "constexpr int *p1 = nullptr;\n"
        "constexpr int *p2 = 0;\n"
        "int main(void) { return p1 == p2 ? 0 : 1; }\n",
    });
    EXPECT_FALSE(okModel.hasErrors())
        << "nullptr and integer-0 are null pointer constants (C 6.3.2.3p3)";
    auto badModel = analyzeShipped("c-subset", {
        "int g;\n"
        "constexpr int *p = &g;\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(badModel.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 1u)
        << "&g is an address constant, not a supported constexpr pointer init";
}

// Aggregate deferral (D-CSUBSET-CONSTEXPR-AGGREGATE-TYPE): array/struct/union
// constexpr objects fail loud — a UNIFORM boundary (the char-array string form
// is deliberately not carved out).
TEST(SemanticAnalyzerCSubset, ConstexprAggregateTypesFailLoud) {
    auto arrModel = analyzeShipped("c-subset", {
        "constexpr int a[3] = {1, 2, 3};\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(arrModel.diagnostics(),
                        DiagnosticCode::S_ConstexprUnsupportedType), 1u);
    auto strModel = analyzeShipped("c-subset", {
        "constexpr char s[] = \"hi\";\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(strModel.diagnostics(),
                        DiagnosticCode::S_ConstexprUnsupportedType), 1u)
        << "the char-array-from-string form keeps the UNIFORM boundary";
    auto structModel = analyzeShipped("c-subset", {
        "struct S { int x; };\n"
        "struct S s0;\n"
        "int main(void) { constexpr struct S s = s0; return 0; }\n",
    });
    EXPECT_EQ(countCode(structModel.diagnostics(),
                        DiagnosticCode::S_ConstexprUnsupportedType), 1u);
}

// isConstexpr IMPLIES isConst end-to-end: assigning to a constexpr object is
// rejected by the EXISTING const-violation machinery (code-audit MEDIUM-3 pin —
// the flag pin alone doesn't prove the assignment path fires).
TEST(SemanticAnalyzerCSubset, ConstexprAssignmentRejected) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) { constexpr int x = 5; x = 6; return x; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConstViolation), 1u)
        << "a constexpr object is implicitly const — assignment must reject";
}

// ACCEPTING-PIN (D-CSUBSET-CONSTEXPR-EXACT-REPRESENTABILITY, code-audit
// MEDIUM-1): C23 6.7.1p10 requires the initializer's value be EXACTLY
// representable in the declared type — `constexpr int x = 1.5;` and
// `constexpr unsigned u = -1;` are constraint violations under GCC/Clang.
// DSS currently ACCEPTS both (the fold succeeds; the stored value matches the
// plain-const equivalent — a silent-accept of invalid C23, NOT a miscompile).
// This pin DOCUMENTS the boundary; when the deferral closes, it flips red and
// is updated deliberately (the gated-deferral discipline).
TEST(SemanticAnalyzerCSubset, ConstexprExactRepresentabilityCurrentlyUnenforced) {
    auto fracModel = analyzeShipped("c-subset", {
        "int main(void) { constexpr int x = 1.5; return x; }\n",
    });
    EXPECT_FALSE(fracModel.hasErrors())
        << "documents the OPEN 6.7.1p10 boundary — update when the deferral closes";
    auto negModel = analyzeShipped("c-subset", {
        "int main(void) { constexpr unsigned int u = -1; return 0; }\n",
    });
    EXPECT_FALSE(negModel.hasErrors())
        << "documents the OPEN 6.7.1p10 boundary — update when the deferral closes";
}

// ── FC17.5 (D-CSUBSET-EMPTY-INITIALIZER + D-CSUBSET-FUNC-PREDEFINED-
//    IDENTIFIER): C23 {} empty/scalar brace-init × constexpr, and the C99
//    6.4.2.2 `__func__` predefined identifier ────────────────────────────────

// F3 (C23 6.7.10p11 × 6.7.1): an EMPTY brace initializer zero-initializes —
// zero is a valid compile-time value for every scalar constexpr object,
// including the pointer arm (zero = the null pointer constant). Without the
// F3 empty-brace arm, `constExprValue` cannot fold `{}` (it is not an
// expression) and both declarations would false-fire
// S_ConstexprNonConstantInitializer.
TEST(SemanticAnalyzerCSubset, ConstexprEmptyBraceInitializerValid) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    constexpr int x = {};\n"
        "    constexpr int *p = {};\n"
        "    return x + (p == 0 ? 0 : 1);\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
}

// The now-green DELTA pin: `constexpr int x = {5};` passes semantic — the
// single-element brace list folds through the normal single-child descent
// (the HIR scalar brace-init lift makes the whole program compile; this pin
// holds the SEMANTIC half green).
TEST(SemanticAnalyzerCSubset, ConstexprSingleBraceInitializerValid) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) { constexpr int x = {5}; return x; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
}

// F1 (C99 6.4.2.2): the synthetic `__func__` symbol is `isConst`, so SE4's
// const check rejects simple assignment — the isConst flag is the ONLY guard
// on this path (pin it; without it `__func__ = x` would reach HIR lowering
// and dead-end as a rodata write).
TEST(SemanticAnalyzerCSubset, FuncNameAssignmentIsConstViolation) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    const char *x;\n"
        "    x = __func__;\n"
        "    __func__ = x;\n"
        "    return 0;\n"
        "}\n",
    });
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConstViolation), 1u)
        << "__func__ = x must reject via the synthetic symbol's isConst";
}

// F1: compound assignment takes the SAME SE4 const chokepoint (the
// operator-gated assignment entries share the rule) — `__func__ += 1` is a
// distinct classifier path from plain `=`, so pin it separately.
TEST(SemanticAnalyzerCSubset, FuncNameCompoundAssignIsConstViolation) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) { __func__ += 1; return 0; }\n",
    });
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConstViolation), 1u)
        << "__func__ += 1 must reject via the synthetic symbol's isConst";
}

// N4: the synthetic binds BEFORE the params (Pass 1 binds it when the
// function's scope is pushed; the params bind as the driver walks the
// children AFTER), so a param named `__func__` collides at ITS OWN bind —
// a positioned S_RedeclaredSymbol, never a crash or a silent shadow.
TEST(SemanticAnalyzerCSubset, ParamNamedFuncNameRedeclares) {
    auto model = analyzeShipped("c-subset", {
        "int f(int __func__) { return __func__; }\n"
        "int main(void) { return f(42); }\n",
    });
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "a param named __func__ must collide with the earlier synthetic "
           "bind (N4: bind-before-params)";
}

// The binding is FUNCTION-scoped (C99 6.4.2.2 declares __func__ inside each
// function definition): at FILE scope there is no enclosing function and the
// name resolves to NOTHING — a use fails loud as an ordinary undeclared
// identifier, never a guessed global.
TEST(SemanticAnalyzerCSubset, FuncNameOutsideFunctionIsUndeclared) {
    auto model = analyzeShipped("c-subset", {
        "int x = __func__[0];\n"
        "int main(void) { return x; }\n",
    });
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_UndeclaredIdentifier), 1u)
        << "__func__ at file scope must be undeclared (the binding is "
           "per-function-definition)";
}

// ── FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS, C23 6.7.13): standard-attribute
//    semantics — maybe_unused / deprecated / nodiscard / fallthrough /
//    unknown-attribute policy ─────────────────────────────────────────────────

// C23 6.7.13.4: `[[maybe_unused]]` suppresses the D8 unused-variable warning.
// The SAME-shape unflagged sibling `y` still warns — the paired control makes
// this red-on-disable by construction (drop the D8 `isMaybeUnused` skip → the
// count becomes 2; drop the scan/mint → 2; break the D8 gate itself → 0 and
// the control flips).
TEST(SemanticAnalyzerCSubset, MaybeUnusedSuppressesUnusedWarning) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    [[maybe_unused]] int x = 5;\n"
        "    int y = 6;\n"
        "    return 0;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 1u)
        << "exactly the unflagged sibling warns";
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_UnusedVariable)
            EXPECT_EQ(diag.actual, "y")
                << "the [[maybe_unused]] x must not be the warning subject";
    }
}

// The GNU spelling `__attribute__((unused))` maps to the SAME suppressUnused
// row (dunder-normalized, so `__unused__` also matches). Block scope: the
// attrSpec rides varDecl's localDeclSpecifiers.
TEST(SemanticAnalyzerCSubset, GnuUnusedSpellingSuppresses) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    __attribute__((unused)) int x = 5;\n"
        "    __attribute__((__unused__)) int w = 7;\n"
        "    return 0;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u)
        << "both GNU unused spellings must suppress the warning";
}

// C23 6.7.13: an attribute in the declaration specifiers appertains to EACH
// declared entity — `[[maybe_unused]] int a, b;` suppresses for BOTH
// declarators (the facts are folded once per declaration, applied per
// declarator — the alignas/noreturn shared-prefix precedent).
TEST(SemanticAnalyzerCSubset, MaybeUnusedMultiDeclaratorAppliesAll) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    [[maybe_unused]] int a, b;\n"
        "    return 0;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u)
        << "the shared specifier prefix must flag every declarator";
}

// C23 6.7.13.3: each USE of a `[[deprecated]]` function warns — per call site
// (2 calls → 2 warnings, distinct spans), as a WARNING, naming the symbol. A
// non-deprecated sibling `h` never warns (the control).
TEST(SemanticAnalyzerCSubset, DeprecatedWarnsAtEachCallSite) {
    auto model = analyzeShipped("c-subset", {
        "[[deprecated]] int g(void) { return 1; }\n"
        "int h(void) { return 2; }\n"
        "int main(void) { return g() + h() + g(); }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 2u)
        << "one warning per use site — two calls to g, none for h";
    std::vector<std::uint32_t> starts;
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code != DiagnosticCode::S_DeprecatedSymbolUsed) continue;
        EXPECT_EQ(diag.severity, DiagnosticSeverity::Warning);
        EXPECT_EQ(diag.actual, "g");
        starts.push_back(diag.span.start());
    }
    ASSERT_EQ(starts.size(), 2u);
    EXPECT_NE(starts[0], starts[1])
        << "the two warnings must anchor at the two DISTINCT use sites";
}

// `[[deprecated("use h instead")]]` — the decoded string argument rides the
// warning as `name: msg` (the shared decodeAdjacentStringBodies chokepoint).
TEST(SemanticAnalyzerCSubset, DeprecatedMessageIncluded) {
    auto model = analyzeShipped("c-subset", {
        "[[deprecated(\"use h instead\")]] int g(void) { return 1; }\n"
        "int main(void) { return g(); }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u);
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_DeprecatedSymbolUsed)
            EXPECT_EQ(diag.actual, "g: use h instead");
    }
}

// A `[[deprecated]]` PROTOTYPE + an unflagged definition: the flag OR-merges
// into the surviving definition (the isNoreturn mergedFnDecls precedent), so a
// call — which resolves to the survivor — still warns. RED-ON-DISABLE for the
// merge: drop the OR-merge block → detection marked only the absorbed proto,
// the survivor stays unflagged, the count drops to 0.
TEST(SemanticAnalyzerCSubset, DeprecatedProtoOrMergesIntoDefinition) {
    auto model = analyzeShipped("c-subset", {
        "[[deprecated(\"legacy\")]] int g(void);\n"
        "int g(void) { return 1; }\n"
        "int main(void) { return g(); }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u)
        << "the proto's deprecated flag must OR-merge into the definition";
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_DeprecatedSymbolUsed)
            EXPECT_EQ(diag.actual, "g: legacy")
                << "the message must merge too (first-non-empty-wins)";
    }
}

// A deprecated OBJECT (not just functions): each use of the global warns via
// the same reference-resolution chokepoint.
TEST(SemanticAnalyzerCSubset, DeprecatedObjectUseWarns) {
    auto model = analyzeShipped("c-subset", {
        "[[deprecated]] int legacy_flag;\n"
        "int main(void) { return legacy_flag; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u)
        << "a deprecated object's use site must warn like a function's";
}

// C23 6.7.13.2: a `[[nodiscard]]` call whose result is DISCARDED — the call is
// the entire expression of an expression statement — warns (Warning severity,
// naming the callee).
TEST(SemanticAnalyzerCSubset, NodiscardDiscardedWarns) {
    auto model = analyzeShipped("c-subset", {
        "[[nodiscard]] int f(void) { return 1; }\n"
        "int main(void) { f(); return 0; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NodiscardResultDiscarded), 1u);
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_NodiscardResultDiscarded) {
            EXPECT_EQ(diag.severity, DiagnosticSeverity::Warning);
            EXPECT_EQ(diag.actual, "f");
        }
    }
}

// ★ THE F1 red-on-disable pin: `(void)f();` must NOT warn. The discard check
// is TWO-hop-exact (parent(call)==expression AND grandparent==exprStmt) at the
// SEMANTIC tier where the cast still exists structurally (a castExpr
// interposes → parent≠expression-under-exprStmt). This pin goes red if the
// check ever moves post-HIR (where the (void) cast is elided) OR if the hop
// count is wrong (a THREE-hop / suffix-blind check would fire here; the
// original ONE-hop design bug would fire NOWHERE — caught by
// NodiscardDiscardedWarns above going red instead).
TEST(SemanticAnalyzerCSubset, NodiscardVoidCastSuppresses) {
    auto model = analyzeShipped("c-subset", {
        "[[nodiscard]] int f(void) { return 1; }\n"
        "int main(void) { (void)f(); return 0; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NodiscardResultDiscarded), 0u)
        << "the (void) cast is the C idiom for a deliberate discard — no warning";
}

// A nodiscard result that IS consumed never warns: initializer, argument, and
// return-value positions (each has the wrong parent/grandparent shape).
TEST(SemanticAnalyzerCSubset, NodiscardUsedInExpressionNoWarn) {
    auto model = analyzeShipped("c-subset", {
        "[[nodiscard]] int f(void) { return 1; }\n"
        "int g(int v) { return v; }\n"
        "int main(void) {\n"
        "    int r = f();\n"
        "    int s = g(f());\n"
        "    if (r + s == 3) { return f(); }\n"
        "    return 0;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NodiscardResultDiscarded), 0u)
        << "init / argument / return positions all consume the result";
}

// `[[nodiscard("reason")]]` — the message rides the warning as `name: msg`.
TEST(SemanticAnalyzerCSubset, NodiscardMessageIncluded) {
    auto model = analyzeShipped("c-subset", {
        "[[nodiscard(\"check the error code\")]] int f(void) { return 1; }\n"
        "int main(void) { f(); return 0; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NodiscardResultDiscarded), 1u);
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_NodiscardResultDiscarded)
            EXPECT_EQ(diag.actual, "f: check the error code");
    }
}

// The GNU spelling `__attribute__((warn_unused_result))` maps to the SAME
// warnOnDiscard row; a proto-only spelling OR-merges into the definition
// (message-less → `.actual` is the bare name).
TEST(SemanticAnalyzerCSubset, GnuWarnUnusedResultWarnsAndMerges) {
    auto model = analyzeShipped("c-subset", {
        "__attribute__((warn_unused_result)) int f(void);\n"
        "int f(void) { return 1; }\n"
        "int main(void) { f(); return 0; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NodiscardResultDiscarded), 1u)
        << "the GNU spelling + the proto-to-definition OR-merge must both work";
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_NodiscardResultDiscarded)
            EXPECT_EQ(diag.actual, "f");
    }
}

// C23 6.7.13p3 posture: an UNKNOWN `[[...]]` standard attribute warns
// SUPPRESSIBLY and the program still compiles (hasErrors()==false) — C23
// forbids treating it as fatal. Exactly ONE warning even for a
// multi-declarator declaration (the once-per-declaration scan site).
TEST(SemanticAnalyzerCSubset, UnknownStdAttrWarnsSuppressibly) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    [[frobnicate]] int x = 1, y = 2;\n"
        "    return x + y - 3;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "an unknown standard attribute must never fail the build";
    ASSERT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownAttribute), 1u)
        << "once per declaration, not per declarator";
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_UnknownAttribute) {
            EXPECT_EQ(diag.severity, DiagnosticSeverity::Warning);
            EXPECT_EQ(diag.actual, "frobnicate");
        }
    }
}

// ★ THE F4 pin: names the language KNOWS — consumed by a dedicated scan
// (noreturn) or deliberately inert per C23 (fallthrough/likely/unlikely/
// reproducible/unsequenced) — must NOT trip the unknown-attribute warning.
// Without the effect-table `none` rows, `[[noreturn]] int f(void);` would
// false-fire S_UnknownAttribute.
TEST(SemanticAnalyzerCSubset, KnownC23NoOpAttributesDontWarn) {
    auto model = analyzeShipped("c-subset", {
        "[[noreturn]] void die(void);\n"
        "void die(void) { while (1) { } }\n"
        "[[reproducible]] int f(void) { return 1; }\n"
        "int main(void) {\n"
        "    [[likely]] int a = f();\n"
        "    [[unlikely]] int b = 2;\n"
        "    [[unsequenced]] int c = 3;\n"
        "    return a + b + c - 6;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownAttribute), 0u)
        << "known C23 vocabulary must never warn unknown (F4)";
}

// C23 6.8.1: the bare attribute-declaration STATEMENT `[[fallthrough]];`
// parses + analyzes clean inside a switch (both spellings). The runtime
// witness (1+10=11 through the marked fallthrough) is the
// examples/c-subset/switch_fallthrough_attribute corpus entry.
TEST(SemanticAnalyzerCSubset, FallthroughStatementParsesInSwitch) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    int x = 1; int acc = 0;\n"
        "    switch (x) {\n"
        "        case 1: acc += 1; [[fallthrough]];\n"
        "        case 2: acc += 10; break;\n"
        "        default: acc = 99; break;\n"
        "    }\n"
        "    return acc - 11;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "[[fallthrough]]; must parse + analyze clean as a switch body item";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownAttribute), 0u);
}

// The GNU statement spelling `__attribute__((fallthrough));` rides the SAME
// attributeDeclaration rule (compositeAttrList admits attrSpec | stdAttr).
TEST(SemanticAnalyzerCSubset, GnuFallthroughSpellingParses) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    int x = 1; int acc = 0;\n"
        "    switch (x) {\n"
        "        case 1: acc += 1; __attribute__((fallthrough));\n"
        "        case 2: acc += 10; break;\n"
        "        default: acc = 99; break;\n"
        "    }\n"
        "    return acc - 11;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "__attribute__((fallthrough)); must parse as a statement";
}

// A bare statement with an UNKNOWN standard attribute (`[[frobnicate]];`)
// warns suppressibly through the pass2Post bareStatementRule arm — same
// policy as the declaration position, still no error.
TEST(SemanticAnalyzerCSubset, UnknownBareStatementAttrWarnsSuppressibly) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    [[frobnicate]];\n"
        "    return 0;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownAttribute), 1u);
}

// ── FC17.5 (D-CSUBSET-AUTO-TYPE-INFERENCE): C23 6.7.9 `auto` type inference ──
//
// The feature: `auto x = expr;` — a HEAD-LESS declaration whose type is
// INFERRED from the initializer at the declaration's own Pass-1.5 visit.
// The block pins the three design-audit CRITICALs: ★C1 the auto-presence
// gate (C89 implicit-int shapes stay errors), ★C2 the branch order (the
// >4096-token committed-replay pin below), ★C3 the full inference
// normalization (decay / loud rejects / stripVolatile / Pass-1.5 stamps).

namespace {
// Find the FIRST symbol spelled `name`, or nullptr.
[[nodiscard]] SymbolRecord const*
findSymbolNamed(SemanticModel const& model, std::string_view name) {
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == name) return &model.symbols()[i];
    }
    return nullptr;
}
} // namespace

// THE Pass-1.5-VISIBILITY pin (★C3): the inferred type must be written at
// the declaration's OWN Pass-1.5 visit, not backfilled from the initializer
// at Pass 2. The discriminator is a FOLLOWING `typeof(x) y;` declaration:
// typeof's expression operand resolves at y's own Pass-1.5 visit by reading
// x's SYMBOL TYPE (subtreeType's scope-lookup leaf), and y carries NO
// initializer, so the Pass-2 backfill can never type it. Under a
// backfill-only implementation x is untyped when y resolves → y stays
// untyped → the y->type assert reds. RED-ON-ARM-DISABLE verified.
// (The `int arr[sizeof(x)]` form of this pin lives in the RUNNABLE example
// `auto_type_inference` — the raw-analyze fixture has a PRE-EXISTING
// sizeof-of-LOCAL-in-array-dim limitation pinned by the
// `sizeof_value_in_array_dim` corpus golden [S_NonConstantArrayLength even
// for `const int x = 7;`], which the full CLI pipeline does not share; the
// example compiles + runs 42 through the real driver on debug AND release.)
TEST(SemanticAnalyzerCSubset, AutoInfersIntPass15Visible) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    auto x = 42;\n"
        "    typeof(x) y;\n"
        "    y = 1;\n"
        "    return y + x;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "the inferred type must be Pass-1.5-visible (typeof(x) in the "
           "next declaration)";
    auto const* x = findSymbolNamed(model, "x");
    ASSERT_NE(x, nullptr);
    ASSERT_TRUE(x->type.valid());
    EXPECT_EQ(model.lattice().interner().kind(x->type), TypeKind::I32);
    auto const* y = findSymbolNamed(model, "y");
    ASSERT_NE(y, nullptr);
    ASSERT_TRUE(y->type.valid())
        << "typeof(x) at the NEXT declaration's Pass-1.5 visit must see the "
           "inferred type (y has no initializer — the backfill cannot type it)";
    EXPECT_EQ(model.lattice().interner().kind(y->type), TypeKind::I32);
}

// ★C1 — the auto-presence gate: all four specifier-led C89 implicit-int
// shapes PARSE into the headless rule and MUST stay errors
// (S_AutoInferenceInvalid, one per declaration). RED-ON-DISABLE: drop the
// row's requiredSpecifierToken (or the arm's gate) and each silently
// becomes an initializer-typed declaration.
TEST(SemanticAnalyzerCSubset, AutoPresenceGateKeepsImplicitIntErrors) {
    char const* const forms[] = {
        "int main(void) { static x = 5; return x; }\n",
        "int main(void) { register y = 2; return y; }\n",
        "int main(void) { alignas(4) z = 9; return z; }\n",
        "int main(void) { [[maybe_unused]] w = 3; return w; }\n",
    };
    for (auto const* src : forms) {
        auto model = analyzeShipped("c-subset", {std::string{src}});
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_AutoInferenceInvalid), 1u)
            << "C89 implicit-int must stay an error for: " << src;
    }
}

// ★C3 decay — a string-literal initializer infers char* (NOT Array<Char,4>):
// the un-decayed array would give sizeof(s)==4 and a wrong-typed object.
TEST(SemanticAnalyzerCSubset, AutoStringLiteralDecaysToCharPointer) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) { auto s = \"str\"; return s[0] == 's' ? 0 : 1; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* s = findSymbolNamed(model, "s");
    ASSERT_NE(s, nullptr);
    ASSERT_TRUE(s->type.valid());
    auto const& in = model.lattice().interner();
    ASSERT_EQ(in.kind(s->type), TypeKind::Ptr) << "array-to-pointer decay";
    EXPECT_EQ(in.kind(in.operands(s->type)[0]), TypeKind::Char);
}

// ★C3 decay — an array VARIABLE initializer decays to pointer-to-element.
TEST(SemanticAnalyzerCSubset, AutoArrayVariableDecaysToElementPointer) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    int a[3];\n"
        "    a[1] = 7;\n"
        "    auto p = a;\n"
        "    return p[1];\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* p = findSymbolNamed(model, "p");
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(p->type.valid());
    auto const& in = model.lattice().interner();
    ASSERT_EQ(in.kind(p->type), TypeKind::Ptr);
    EXPECT_EQ(in.kind(in.operands(p->type)[0]), TypeKind::I32);
}

// ★C3 decay — a function-name initializer decays to pointer-to-function
// (the c56 fn-designator precedent), and the object is callable.
TEST(SemanticAnalyzerCSubset, AutoFunctionNameDecaysToFunctionPointer) {
    auto model = analyzeShipped("c-subset", {
        "static int twice(int v) { return v + v; }\n"
        "int main(void) { auto f = twice; return f(21); }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* f = findSymbolNamed(model, "f");
    ASSERT_NE(f, nullptr);
    ASSERT_TRUE(f->type.valid());
    auto const& in = model.lattice().interner();
    ASSERT_EQ(in.kind(f->type), TypeKind::Ptr) << "function-to-pointer decay";
    EXPECT_EQ(in.kind(in.operands(f->type)[0]), TypeKind::FnSig);
}

// C23 6.7.9p2 — exactly ONE declarator.
TEST(SemanticAnalyzerCSubset, AutoMultiDeclaratorRejected) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) { auto a = 1, b = 2; return a + b; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AutoRequiresSingleDeclarator), 1u);
}

// C23 6.7.9p2 — a PLAIN IDENTIFIER declarator. `auto *p = 0;` (a derived
// declarator — D-CSUBSET-AUTO-DERIVED-DECLARATOR) and the diagnostic-SHAPE
// pin `auto f(void);` (parses into the headless rule; must be THIS code,
// never a silent prototype) both reject S_AutoRequiresPlainIdentifier.
TEST(SemanticAnalyzerCSubset, AutoDerivedDeclaratorRejected) {
    auto ptrModel = analyzeShipped("c-subset", {
        "int main(void) { auto *p = 0; return 0; }\n",
    });
    EXPECT_EQ(countCode(ptrModel.diagnostics(),
                        DiagnosticCode::S_AutoRequiresPlainIdentifier), 1u);
    auto fnModel = analyzeShipped("c-subset", {
        "int main(void) { auto f(void); return 0; }\n",
    });
    EXPECT_EQ(countCode(fnModel.diagnostics(),
                        DiagnosticCode::S_AutoRequiresPlainIdentifier), 1u);
}

// C23 6.7.9p2 — an initializer is REQUIRED. `auto T;` is the second
// diagnostic-SHAPE pin (the one dual-parse shape `<specifiers> Ident ;`
// must surface as THIS inference-tier code).
TEST(SemanticAnalyzerCSubset, AutoMissingInitializerRejected) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) { auto T; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AutoRequiresInitializer), 1u);
}

// ★C3 rejects — void call / bare nullptr / self-reference each fail loud
// S_AutoInferenceInvalid (RED-ON-DISABLE: without the arm's rejects, Pass
// 2's initializer backfill silently adopts Void / NullptrT / nothing).
TEST(SemanticAnalyzerCSubset, AutoUninferableInitializersRejected) {
    auto voidModel = analyzeShipped("c-subset", {
        "void vf(void) { }\n"
        "int main(void) { auto v = vf(); return 0; }\n",
    });
    EXPECT_EQ(countCode(voidModel.diagnostics(),
                        DiagnosticCode::S_AutoInferenceInvalid), 1u)
        << "auto from a void call must reject";
    auto nullModel = analyzeShipped("c-subset", {
        "int main(void) { auto p = nullptr; return 0; }\n",
    });
    EXPECT_EQ(countCode(nullModel.diagnostics(),
                        DiagnosticCode::S_AutoInferenceInvalid), 1u)
        << "auto from bare nullptr must reject (nullptr_t not declarable)";
    auto selfModel = analyzeShipped("c-subset", {
        "int main(void) { auto x = x; return 0; }\n",
    });
    EXPECT_EQ(countCode(selfModel.diagnostics(),
                        DiagnosticCode::S_AutoInferenceInvalid), 1u)
        << "self-referential initializer must reject loud";
}

// C23 6.7.9p2 via 6.7.10p12 — the braced SINGLE form infers; the empty and
// multi-element forms reject via the shared scalar-brace constraint code.
TEST(SemanticAnalyzerCSubset, AutoBracedSingleInfersAndMalformedRejects) {
    auto okModel = analyzeShipped("c-subset", {
        "int main(void) { auto x = {5}; return x - 5; }\n",
    });
    EXPECT_FALSE(okModel.hasErrors());
    auto const* x = findSymbolNamed(okModel, "x");
    ASSERT_NE(x, nullptr);
    ASSERT_TRUE(x->type.valid());
    EXPECT_EQ(okModel.lattice().interner().kind(x->type), TypeKind::I32);
    auto multiModel = analyzeShipped("c-subset", {
        "int main(void) { auto y = {1, 2}; return 0; }\n",
    });
    EXPECT_EQ(countCode(multiModel.diagnostics(),
                        DiagnosticCode::S_InvalidScalarInitializer), 1u);
    auto emptyModel = analyzeShipped("c-subset", {
        "int main(void) { auto z = {}; return 0; }\n",
    });
    EXPECT_EQ(countCode(emptyModel.diagnostics(),
                        DiagnosticCode::S_InvalidScalarInitializer), 1u)
        << "`auto z = {};` has no expression to infer from";
}

// The C89 REGRESSION pin: `auto int x;` (auto as a plain storage-class with
// a real type head) must keep parsing via varDecl on rollback — the
// inference rule fast-fails on the `int` and the committed path types x int.
TEST(SemanticAnalyzerCSubset, AutoC89StorageClassFormUnchanged) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) { auto int x; x = 42; return x; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* x = findSymbolNamed(model, "x");
    ASSERT_NE(x, nullptr);
    ASSERT_TRUE(x->type.valid());
    EXPECT_EQ(model.lattice().interner().kind(x->type), TypeKind::I32);
}

// ★C3 stripVolatile — a volatile-typed initializer infers the UNQUALIFIED
// type (C23 6.7.9p2 drops top-level qualifiers; the typeof_unqual strip).
TEST(SemanticAnalyzerCSubset, AutoTopLevelVolatileStripped) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    volatile int v;\n"
        "    v = 1;\n"
        "    auto x = v;\n"
        "    x = 2;\n"
        "    return x + v;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* x = findSymbolNamed(model, "x");
    ASSERT_NE(x, nullptr);
    ASSERT_TRUE(x->type.valid());
    EXPECT_EQ(model.lattice().interner().kind(x->type), TypeKind::I32)
        << "the inferred type must be the STRIPPED I32, not VolatileQual(I32)";
}

// constexpr composes with the inference (P1 prefix scan -> P1.5 infer ->
// P2 constexpr validation reads the INFERRED type): the object folds as an
// integer constant expression and carries both Pass-1 marks.
TEST(SemanticAnalyzerCSubset, AutoConstexprComposes) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) { constexpr auto k = 6; int a[k]; a[0] = 1; "
        "return a[0]; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* k = findSymbolNamed(model, "k");
    ASSERT_NE(k, nullptr);
    EXPECT_TRUE(k->isConstexpr);
    EXPECT_TRUE(k->isConst);
    auto const* a = findSymbolNamed(model, "a");
    ASSERT_NE(a, nullptr);
    ASSERT_TRUE(a->type.valid());
    auto const& in = model.lattice().interner();
    ASSERT_EQ(in.kind(a->type), TypeKind::Array);
    EXPECT_EQ(in.scalars(a->type)[0], 6);
}

// ★C2 — THE BRANCH-ORDER pin: a >4096-token block-scope `static const int
// big[] = {…}` must still compile. With autoInferredVarDecl declared FIRST,
// varDecl is the declared-LAST structural candidate for a specifier-led
// statement, so when every speculative probe fails (autoInferred fast-fails
// at `const`; varDecl exhausts the 4096-token probe budget on the huge
// initializer) the parser's all-fail REPLAY re-parses varDecl
// NON-speculatively with no budget — a genuine committed parse.
// RED-ON-REORDER (empirically verified at implement time): with
// [varDecl, autoInferredVarDecl, …] the replay target is the inference rule,
// which cannot parse `const` → P0009. sqlite3.c's largest block-scope static
// init is 3918 tokens = 96% of the budget, so the sqlite gate can NOT catch
// this cliff — only this pin does.
TEST(SemanticAnalyzerCSubset, AutoHugeStaticInitParsesViaCommittedReplay) {
    std::string src = "int main(void) {\n    static const int big[] = {";
    for (int i = 0; i < 2200; ++i) {          // ~4400 tokens inside the braces
        if (i > 0) src += ',';
        src += std::to_string(i % 97);
    }
    src += "};\n    return big[3];\n}\n";
    auto model = analyzeShipped("c-subset", {src});
    EXPECT_FALSE(model.hasErrors())
        << "a >4096-token static initializer must parse via the committed "
           "replay of the declared-LAST varDecl branch";
    auto const* big = findSymbolNamed(model, "big");
    ASSERT_NE(big, nullptr);
    ASSERT_TRUE(big->type.valid());
    auto const& in = model.lattice().interner();
    ASSERT_EQ(in.kind(big->type), TypeKind::Array);
    EXPECT_EQ(in.scalars(big->type)[0], 2200);
}

// The for-init mirror: `for (auto i = 0; …)` infers (C23 6.8.5p3 admits
// auto in a for-init) and `for (static auto i = 0;;)` stays gated loud
// (the copied forDecl StaticKeyword gatedMarker — C 6.8.5p3 violation).
TEST(SemanticAnalyzerCSubset, AutoForInitInfersAndStaticStaysGated) {
    auto okModel = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    int acc = 0;\n"
        "    for (auto i = 0; i < 7; i = i + 1) acc = acc + i;\n"
        "    return acc;\n"
        "}\n",
    });
    EXPECT_FALSE(okModel.hasErrors());
    auto const* i = findSymbolNamed(okModel, "i");
    ASSERT_NE(i, nullptr);
    ASSERT_TRUE(i->type.valid());
    EXPECT_EQ(okModel.lattice().interner().kind(i->type), TypeKind::I32);
    auto gatedModel = analyzeShipped("c-subset", {
        "int main(void) { for (static auto i = 0; i < 3; i = i + 1) { } "
        "return 0; }\n",
    });
    EXPECT_EQ(countCode(gatedModel.diagnostics(),
                        DiagnosticCode::S_StaticStorageInForInit), 1u);
}

// The two NAMED loud parse boundaries stay loud (never silent):
// file-scope `auto g = 42;` (C23 ALLOWS it — D-CSUBSET-AUTO-FILE-SCOPE is
// the named deferral; DSS keeps the pre-existing loud parse reject) and the
// qualified forms `const auto` / `auto const` (D-CSUBSET-AUTO-QUALIFIED).
TEST(SemanticAnalyzerCSubset, AutoFileScopeAndQualifiedStayLoudParseErrors) {
    char const* const rejects[] = {
        "auto g = 42;\nint main(void) { return g; }\n",
        "int main(void) { const auto x = 5; return x; }\n",
        "int main(void) { auto const x = 5; return x; }\n",
    };
    for (auto const* src : rejects) {
        auto cu = buildShippedUnit("c-subset", {std::string{src}});
        bool anyParseError = false;
        for (auto const& t : cu->trees()) {
            for (auto const& d : t.diagnostics().all()) {
                if (d.severity == DiagnosticSeverity::Error) {
                    anyParseError = true;
                }
            }
        }
        EXPECT_TRUE(anyParseError)
            << "must stay a loud parse error (named deferral): " << src;
    }
}

// Positive inference-KIND breadth (code-audit fold): the inferred type is
// pinned EXACTLY for each non-decaying initializer class the arm passes
// through unchanged — a struct variable (aggregates infer by value, no
// decay), an enumerator (the enum TYPE, not its underlying int), a
// comparison (Bool — promoteComparisons), a char variable (Char, not the
// promoted int), and an unsuffixed float literal (F64 per C 6.4.4.2). All
// in ONE unit so the block also witnesses the inferred objects USED
// together (member access through the inferred struct, arithmetic across
// the rest).
TEST(SemanticAnalyzerCSubset, AutoInfersExactKindsAcrossValueClasses) {
    auto model = analyzeShipped("c-subset", {
        "struct S { int x; };\n"
        "enum E { A };\n"
        "int main(void) {\n"
        "    struct S sv;\n"
        "    sv.x = 1;\n"
        "    auto s2 = sv;\n"
        "    auto e = A;\n"
        "    auto b = (1 < 2);\n"
        "    char cv = 'a';\n"
        "    auto c = cv;\n"
        "    auto f = 2.5;\n"
        "    return s2.x + e + b + c + (int)f;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const& in = model.lattice().interner();
    auto const kindOf = [&](char const* name) -> TypeKind {
        auto const* rec = findSymbolNamed(model, name);
        if (rec == nullptr || !rec->type.valid()) {
            ADD_FAILURE() << "symbol '" << name << "' missing or untyped";
            return TypeKind::Void;
        }
        return in.kind(rec->type);
    };
    EXPECT_EQ(kindOf("s2"), TypeKind::Struct)
        << "a struct variable infers the struct type BY VALUE (no decay)";
    EXPECT_EQ(kindOf("e"), TypeKind::Enum)
        << "an enumerator infers the ENUM type (enumConvertsToArith covers "
           "its uses; the type itself stays Enum)";
    EXPECT_EQ(kindOf("b"), TypeKind::Bool)
        << "a comparison infers Bool (promoteComparisons)";
    EXPECT_EQ(kindOf("c"), TypeKind::Char)
        << "a char VARIABLE infers Char (the symbol's type, not the "
           "promoted int)";
    EXPECT_EQ(kindOf("f"), TypeKind::F64)
        << "an unsuffixed float literal infers double (C 6.4.4.2)";
}

// ── TLS C1 (D-CSUBSET-THREAD-LOCAL): C11/C23 6.7.1 thread storage duration ──
//
// The ACCEPT matrix: every legal spelling/placement parses AND marks the
// symbol record. RED-ON-DISABLE: drop the Pass-1 `scanSpecifierPrefixStorage`
// mint (or the linkageSpecifiers `{threadStorage:true}` config entries) and
// every isThreadLocal EXPECT below reds — and with it every enforcement test
// in this block stops firing (the validator gates on the mark).
TEST(SemanticAnalyzerCSubset, ThreadLocalAcceptsAndMarksSymbols) {
    auto model = analyzeShipped("c-subset", {
        "_Thread_local int g = 5;\n"                     // C11 spelling
        "thread_local int h;\n"                          // C23 spelling, tentative
        "static thread_local int s = 2;\n"               // static first
        "thread_local static int s2 = 3;\n"              // thread_local first
        "extern thread_local int e;\n"                   // the cross-TU form
        "int plain = 9;\n"                               // control: unmarked
        "int main(void) {\n"
        "    static thread_local int ls = 4;\n"          // block-scope static
        "    return g + h + s + s2 + ls + plain;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "every accept-matrix form is legal C11/C23";
    for (char const* nm : {"g", "h", "s", "s2", "e", "ls"}) {
        auto const* rec = findSymbolNamed(model, nm);
        ASSERT_NE(rec, nullptr) << nm;
        EXPECT_TRUE(rec->isThreadLocal)
            << nm << " must carry the Pass-1 thread-storage mark";
    }
    auto const* plainRec = findSymbolNamed(model, "plain");
    ASSERT_NE(plainRec, nullptr);
    EXPECT_FALSE(plainRec->isThreadLocal)
        << "an unmarked global must stay process-shared";
}

// thread_local does NOT change linkage (C11 6.2.2 untouched by 6.7.1): the
// file-scope form keeps EXTERNAL linkage, and a co-present `static` keeps its
// INTERNAL binding in EITHER order (the noreturn linkage-clobber lesson — a
// threadStorage row must never last-wins-overwrite a static's binding).
// What the ANALYZER must guarantee is that both orders survive to the HIR
// tier error-free with the thread mark intact on both symbols (the binding
// axis itself is stamped at HIR lowering by linkageFrom, pinned in the MIR
// lowering tests).
TEST(SemanticAnalyzerCSubset, ThreadLocalDoesNotClobberStaticBinding) {
    auto model = analyzeShipped("c-subset", {
        "static thread_local int a = 1;\n"
        "thread_local static int b = 2;\n"
        "int main(void) { return a + b; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    for (char const* nm : {"a", "b"}) {
        auto const* rec = findSymbolNamed(model, nm);
        ASSERT_NE(rec, nullptr) << nm;
        EXPECT_TRUE(rec->isThreadLocal) << nm;
    }
}

// 6.7.1p4 — objects only. A thread_local FUNCTION (prototype and definition
// forms) fails loud S_ThreadLocalOnFunction. RED-ON-DISABLE: drop the
// validator's FnSig arm and both go green (silently compiling the specifier
// away).
TEST(SemanticAnalyzerCSubset, ThreadLocalOnFunctionFailsLoud) {
    auto proto = analyzeShipped("c-subset", {
        "thread_local int f(void);\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(proto.diagnostics(),
                        DiagnosticCode::S_ThreadLocalOnFunction), 1u)
        << "a thread_local prototype is a 6.7.1p4 constraint violation";
    auto def = analyzeShipped("c-subset", {
        "_Thread_local int f(void) { return 1; }\n"
        "int main(void) { return f(); }\n",
    });
    EXPECT_EQ(countCode(def.diagnostics(),
                        DiagnosticCode::S_ThreadLocalOnFunction), 1u)
        << "a thread_local function DEFINITION violates the same constraint";
}

// 6.7.1p3 — a BLOCK-scope thread_local object requires static or extern.
// The plain block form and the for-init form (where the requirement is
// unsatisfiable — a for-init admits neither) both fail loud
// S_ThreadLocalRequiresStaticOrExtern; the C23 auto-inferred block form is
// caught too (the mark rides the autoInferredVarDecl row's config).
// RED-ON-DISABLE: drop the validator's block-scope arm → the plain form goes
// green as a silent AUTOMATIC (the exact storage-duration miscompile the
// code exists to prevent); drop the forDecl gatedMarkers → the for-init form
// goes green.
TEST(SemanticAnalyzerCSubset, ThreadLocalBlockScopeRequiresStaticOrExtern) {
    auto plain = analyzeShipped("c-subset", {
        "int main(void) { thread_local int x = 1; return x; }\n",
    });
    EXPECT_EQ(countCode(plain.diagnostics(),
                        DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern),
              1u);
    auto forInit = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "    for (thread_local int i = 0; i < 2; i = i + 1) {}\n"
        "    return 0;\n"
        "}\n",
    });
    EXPECT_EQ(countCode(forInit.diagnostics(),
                        DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern),
              1u)
        << "a for-init thread_local can never satisfy 6.7.1p3 (gatedMarkers)";
    auto autoForm = analyzeShipped("c-subset", {
        "int main(void) { thread_local auto x = 5; return x; }\n",
    });
    EXPECT_EQ(countCode(autoForm.diagnostics(),
                        DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern),
              1u)
        << "the C23 auto-inferred block decl is caught by the same check";
    // The LEGAL counterpart pins the check polarity: static satisfies p3.
    auto legal = analyzeShipped("c-subset", {
        "int main(void) { static thread_local auto s = 1; return s; }\n",
    });
    EXPECT_EQ(countCode(legal.diagnostics(),
                        DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern),
              0u)
        << "C23 admits auto beside thread_local; static satisfies 6.7.1p3";
}

// 6.7.1p3 "shall be present in the declaration of every declared name with
// thread storage duration" — a same-TU redeclaration pair disagreeing on the
// specifier fails loud S_ThreadLocalRedeclarationMismatch in BOTH directions.
// RED-ON-DISABLE: drop the merge-site check and both silently merge (half
// the accesses would bind the wrong storage).
TEST(SemanticAnalyzerCSubset, ThreadLocalRedeclarationMismatchBothDirections) {
    auto gained = analyzeShipped("c-subset", {
        "extern int g;\n"
        "thread_local int g = 5;\n"
        "int main(void) { return g; }\n",
    });
    EXPECT_EQ(countCode(gained.diagnostics(),
                        DiagnosticCode::S_ThreadLocalRedeclarationMismatch),
              1u)
        << "plain extern then thread_local definition must mismatch";
    auto lost = analyzeShipped("c-subset", {
        "extern thread_local int g;\n"
        "int g = 5;\n"
        "int main(void) { return g; }\n",
    });
    EXPECT_EQ(countCode(lost.diagnostics(),
                        DiagnosticCode::S_ThreadLocalRedeclarationMismatch),
              1u)
        << "extern thread_local then plain definition must mismatch too";
    // The MATCHED pair is legal — pins the check polarity.
    auto matched = analyzeShipped("c-subset", {
        "extern thread_local int g;\n"
        "thread_local int g = 5;\n"
        "int main(void) { return g; }\n",
    });
    EXPECT_EQ(countCode(matched.diagnostics(),
                        DiagnosticCode::S_ThreadLocalRedeclarationMismatch),
              0u);
    EXPECT_FALSE(matched.hasErrors());
}

// 6.7.1p2 + C23 constexpr rules — forbidden storage-class combinations fail
// loud S_ThreadLocalInvalidCombination: `constexpr thread_local` (both
// orders — the check reads the Pass-1 isConstexpr mark, not token order) and
// `register thread_local` (the config-driven incompatibleSpecifierTokens
// scan). `typedef thread_local` cannot co-occur grammatically (typedefDecl
// has no storage-specifier prefix — a loud parse error, not a semantic
// code). RED-ON-DISABLE: drop the validator's combination arms and all three
// compile silently with one specifier dropped.
TEST(SemanticAnalyzerCSubset, ThreadLocalInvalidCombinationsFailLoud) {
    auto cxFirst = analyzeShipped("c-subset", {
        "constexpr thread_local int c = 5;\n"
        "int main(void) { return c; }\n",
    });
    EXPECT_EQ(countCode(cxFirst.diagnostics(),
                        DiagnosticCode::S_ThreadLocalInvalidCombination), 1u);
    auto cxSecond = analyzeShipped("c-subset", {
        "thread_local constexpr int c = 5;\n"
        "int main(void) { return c; }\n",
    });
    EXPECT_EQ(countCode(cxSecond.diagnostics(),
                        DiagnosticCode::S_ThreadLocalInvalidCombination), 1u)
        << "specifier order must not matter (the mark-based check)";
    auto reg = analyzeShipped("c-subset", {
        "int main(void) { register thread_local int r = 1; return r; }\n",
    });
    EXPECT_EQ(countCode(reg.diagnostics(),
                        DiagnosticCode::S_ThreadLocalInvalidCombination), 1u)
        << "register may not pair with thread_local (6.7.1p2)";
}

// VLA C4a-local (D-CSUBSET-VLA): a pointer-to-VLA assignability compare stays EXACT — a
// FIXED-pointee `int (*p)[5]` initialized from a VLA object `int b[2][n]` (rows int[n])
// is a MISMATCH (`Ptr<int[5]>` vs `array(vlaArray(int),2)`; int[5] != int[n]) and must
// REJECT with S_TypeMismatch, never silently decay-accept. Forward-guard for the deferred
// init form (D-CSUBSET-VLA-PTR-INIT-FORM-TYPING): whatever makes `= b` work must NOT
// weaken this exact-row compare. RED-ON-DISABLE: broaden the type_rules.hpp:371 decay
// branch to ignore the element type → this stops firing.
TEST(SemanticAnalyzerCSubset, PtrToVlaFixedPointeeFromVlaObjectRejects) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "  volatile int vn = 4;\n"
        "  int n = vn;\n"
        "  int b[2][n];\n"
        "  int (*p)[5] = b;\n"   // MISMATCH: rows int[5] != int[n]
        "  return 0;\n"
        "}\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
        << "a fixed-pointee ptr initialized from a VLA object with a different row length "
           "must reject with S_TypeMismatch (the decay compare stays exact)";
}

// VLA C4a-local (D-CSUBSET-VLA): the ptr-to-VLA init-form work (and its deferred fix,
// D-CSUBSET-VLA-PTR-INIT-FORM-TYPING) must NEVER regress ordinary aggregate brace-init —
// `int a[3]={1,2,3}` / nested / a scalar init all stay clean (no false S_TypeMismatch
// from a subtreeType descent into a braceInitList). The CRITICAL-1 control that keeps the
// eventual init-form fix guarded. RED-ON-DISABLE: an unguarded subtreeType override on the
// init-derivation path would descend a brace list to a member literal → this reds.
TEST(SemanticAnalyzerCSubset, LocalAggregateBraceInitStaysCleanNoFalseTypeMismatch) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "  int a[3] = {1, 2, 3};\n"
        "  int nested[2][2] = {{1, 2}, {3, 4}};\n"
        "  int scalar = 9;\n"
        "  int *sp = a;\n"                 // plain array-decay init (must also stay clean)
        "  return a[0] + nested[0][0] + scalar + sp[0];\n"
        "}\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "aggregate brace-init / nested / scalar / array-decay inits must not false-fire "
           "S_TypeMismatch";
    EXPECT_FALSE(model.hasErrors())
        << "the brace-init control program must compile clean";
}

// VLA C4b (D-CSUBSET-VLA): `typedef int R[n]; R a;` — a VLA TYPEDEF object — ACCEPTS at
// the semantic tier (zero diagnostics) AND records its typedef ORIGIN: `a`'s
// `vlaTypedefOrigin` is set to the typedef `R` (so HIR/MIR can copy R's decl-frozen size
// down at a's alloca, C99 §6.7.7p2). RED-ON-DISABLE: revert the resolveDeclTypesPost a→R
// correlation and `vlaTypedefOrigin` stays InvalidSymbol (the field EXPECT below reds) —
// the accept was always semantic, so the recorded ORIGIN is the new, load-bearing bit.
TEST(SemanticAnalyzerCSubset, VlaTypedefObjectAcceptsAndRecordsOrigin) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "  volatile int vn = 3;\n"
        "  int n = vn;\n"
        "  typedef int R[n];\n"
        "  R a;\n"
        "  a[0] = 1;\n"
        "  return a[0];\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a VLA typedef object `R a;` must analyze clean (zero diagnostics)";
    SymbolRecord const* aRec = findSym(model, "a");
    SymbolRecord const* rRec = findSym(model, "R");
    ASSERT_NE(aRec, nullptr);
    ASSERT_NE(rRec, nullptr);
    EXPECT_TRUE(aRec->vlaTypedefOrigin.valid())
        << "`a`'s vlaTypedefOrigin must be SET (the a→R correlation) so HIR/MIR copy R's "
           "frozen size down instead of re-evaluating `n`";
    // The recorded origin must be the typedef R itself (name + Type kind) — not a
    // different same-typed symbol (type-dedup makes vlaArray(int) shared, so identity
    // MUST come from the SymbolId, not the type).
    ASSERT_LT(aRec->vlaTypedefOrigin.v, model.symbols().size());
    EXPECT_EQ(model.symbols()[aRec->vlaTypedefOrigin.v].name, "R")
        << "the recorded origin is the typedef R";
    EXPECT_EQ(model.symbols()[aRec->vlaTypedefOrigin.v].kind, DeclarationKind::Type)
        << "the recorded origin is a typedef (DeclarationKind::Type)";
}

// VLA C4a-param (D-CSUBSET-VLA): a PARAMETER pointer-to-VLA `int (*p)[n]` (n a sibling
// param) now RESOLVES — Option B's DISTINCT `paramDecay` signal builds a `vlaArray` row in
// the pointee, so a call passing a VLA object `int b[2][n]` DECAYS to `int (*)[n]` and
// type-checks (zero S_TypeMismatch). The runtime witness is examples/c-subset/
// c99_vla_ptr_param (a genuine VLA-object caller is a NON-leaf VLA function — a C1b
// deferral — so the runnable example casts a fixed buffer; THIS pin covers the VLA-arg
// decay type-check that the example cannot exercise at runtime). RED-ON-DISABLE: revert the
// paramDecay threading and the pointee never becomes a VLA row → the arg fails the exact
// decay compare → S_TypeMismatch reappears.
TEST(SemanticAnalyzerCSubset, ParamPtrToVlaAcceptsAndVlaArgDecays) {
    auto model = analyzeShipped("c-subset", {
        "int f(int n, int (*p)[n]) { return p[1][0]; }\n"
        "int main(void) {\n"
        "  volatile int vn = 3;\n"
        "  int n = vn;\n"
        "  int b[2][n];\n"
        "  return f(n, b);\n"
        "}\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "a VLA object `int b[2][n]` passed to a `int (*p)[n]` param must DECAY and "
           "type-check (zero S_TypeMismatch) — the ptr-to-VLA param resolves the sibling n";
    EXPECT_FALSE(model.hasErrors())
        << "the parameter pointer-to-VLA program analyzes clean at the semantic tier (the "
           "non-leaf VLA-object caller is a separate MIR-tier deferral, not a semantic error)";
}

// VLA C4a-param FIX-5(a) (D-CSUBSET-VLA): a param pointer to a FIXED-length array
// `int (*p)[5]` must STILL accept — paramDecay must NOT turn a constant-length pointee into
// a VLA. The `[5]` constant-folds and never reaches the nullopt/VLA branch, so the store
// gate (typeContainsVla) does not over-fire. RED-ON-DISABLE: if paramDecay wrongly forced a
// VLA on a constant length, the ptr(array(int,5)) pointee would flip to ptr(vlaArray) and a
// fixed `int b[2][5]` arg would then MISMATCH.
TEST(SemanticAnalyzerCSubset, ParamPtrToFixedArrayStillAccepts) {
    auto model = analyzeShipped("c-subset", {
        "int f(int (*p)[5]) { return p[1][0]; }\n"
        "int main(void) {\n"
        "  int b[2][5];\n"
        "  return f(b);\n"
        "}\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "a fixed-pointee `int (*p)[5]` param accepts a fixed `int b[2][5]` arg";
    EXPECT_FALSE(model.hasErrors())
        << "the fixed-pointee ptr param program must analyze clean";
}

// VLA C4a-param FIX-5(b) (D-CSUBSET-VLA): the ADJUSTED form `int a[][n]` — the outer `[]`
// decays to the pointer, the inner `[n]` is the runtime pointee row (C-equivalent to
// `int (*a)[n]`) — must accept a VLA-object arg (zero S_TypeMismatch). RED-ON-DISABLE:
// without the paramDecay threading the inner `[n]` fails S_NonConstantArrayLength.
TEST(SemanticAnalyzerCSubset, ParamAdjustedArrayOfVlaAccepts) {
    auto model = analyzeShipped("c-subset", {
        "int f(int n, int a[][n]) { return a[1][0]; }\n"
        "int main(void) {\n"
        "  volatile int vn = 3;\n"
        "  int n = vn;\n"
        "  int b[2][n];\n"
        "  return f(n, b);\n"
        "}\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "`int a[][n]` (the adjusted form) must accept a VLA-object arg";
    EXPECT_FALSE(model.hasErrors())
        << "the int a[][n] program must analyze clean";
}

// VLA C4a-param (D-CSUBSET-VLA) regression control: a PLAIN `int a[n]` param must STILL
// decay to `int*` (C 6.7.6.3p7 adjusts the OUTERMOST dim to a pointer). A pointer is
// REASSIGNABLE (`a = q`), an array is not — so this compiles clean iff the paramDecay path
// stripped the transient outermost vlaArray via adjustArrayToPointer. RED-ON-DISABLE: if a
// plain array param stopped decaying (stayed a VLA-array object), `a = q` would fail loud
// (S_TypeMismatch — arrays are not assignable, per the genuine-array control elsewhere).
TEST(SemanticAnalyzerCSubset, PlainVlaArrayParamStillDecaysToPointer) {
    auto model = analyzeShipped("c-subset", {
        "int f(int n, int a[n], int *q) { a = q; return a[0]; }\n"
        "int main(void) { int x = 5; return f(1, &x, &x); }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a plain `int a[n]` param must DECAY to `int*` (a POINTER is reassignable); the "
           "paramDecay path must strip the outermost VLA, never leave a VLA-array object";
}

// VLA C4a-param THE KEY OPTION-B GUARD (D-CSUBSET-VLA): a struct field `int a[n]` (variable
// n) must STILL resolve via the struct-field FAM incompleteArray path -> a sole flexible
// array member -> S_FlexibleArraySoleMember. Option B threads a DISTINCT paramDecay signal
// that a struct field NEVER carries (its config row has allowFlexibleArray, not
// arrayToPointer), so the FAM path is byte-identical. RED-ON-DISABLE: a broad fix that
// routed struct fields through the paramDecay VLA branch would build a vlaArray instead of
// an incompleteArray and this diagnostic vanishes.
TEST(SemanticAnalyzerCSubset, StructFieldVlaSoleMemberStillFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "int n = 4;\n"
        "struct S { int a[n]; };\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_FlexibleArraySoleMember), 1u)
        << "the Option-B no-regression guard: a struct field `int a[n]` (variable n) still "
           "routes through the FAM incompleteArray path -> S_FlexibleArraySoleMember";
}

// VLA C4a-param (D-CSUBSET-VLA-FIXED-ARRAY-ARG-COMPAT, deferred): a FIXED arg `int b[2][2]`
// decays to `ptr(array(int,2))`, which is DISTINCT from the param's `ptr(vlaArray)` — the
// DSS -2 VLA sentinel is STRICTER than C's runtime VLA/fixed pointer compatibility — so it
// rejects with S_TypeMismatch (a fail-loud reject of valid-C, NEVER a miscompile). This
// pins that the accept is scoped to a genuinely VLA-shaped arg. RED-ON-DISABLE: a broadened
// decay compare that ignored the element length would silently accept this mismatch.
TEST(SemanticAnalyzerCSubset, ParamPtrToVlaFixedArgRejects) {
    auto model = analyzeShipped("c-subset", {
        "int f(int n, int (*p)[n]) { return p[1][0]; }\n"
        "int main(void) {\n"
        "  int b[2][2];\n"
        "  return f(2, b);\n"
        "}\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
        << "a FIXED `int b[2][2]` arg (ptr(array(int,2))) is DISTINCT from the param's "
           "ptr(vlaArray) -> S_TypeMismatch (D-CSUBSET-VLA-FIXED-ARRAY-ARG-COMPAT deferral)";
}

// ── VLA C4c (D-CSUBSET-VLA, C99 §6.7.6.2/6.7.6.3): array-PARAMETER `static` / cv-qualifier
//    / `*` decorations. ALL decay the parameter to a bare pointer; a NON-parameter use is a
//    constraint violation (S_ArrayParamQualifierNonParameter, 0xE054). ──

// A `int a[static N]` PARAMETER decays to `int*` (C 6.7.6.3p7) — a pointer is REASSIGNABLE
// (`a = q`), an array is not — so this compiles clean iff `[static N]` was accepted AND the
// array decayed. RED-ON-DISABLE: if `[static N]` failed to parse or decay, `a = q` (or the
// parse) fails. Runtime witness: examples/c-subset/c99_array_param_static.
TEST(SemanticAnalyzerCSubset, ArrayParamStaticDecaysToPointer) {
    auto model = analyzeShipped("c-subset", {
        "int f(int a[static 3], int *q) { a = q; return a[0]; }\n"
        "int main(void) { int x = 5; return f(&x, &x); }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "`int a[static 3]` is a legal parameter decoration (C 6.7.6.3p7) — accept + decay "
           "to `int*` (a reassignable pointer)";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayParamQualifierNonParameter), 0u)
        << "a PARAMETER `[static 3]` is legal — the non-parameter gate must NOT fire";
}

// Every array-parameter decoration — `[static n]` (runtime), `[const 3]`, `[restrict]`,
// `[volatile 3]`, and the `[const static 3]` combo — is legal in a parameter and DECAYS to
// `int*` (a `[restrict]` with NO bound decays like `int a[]`, NEVER a runtime VLA).
// RED-ON-DISABLE: any form that failed to parse or wrongly routed to a VLA errors here.
// (The bare unspecified-size `[*]` form LANDED — see ArrayParamStarFormDecaysCleanNonParamFailsLoud.)
TEST(SemanticAnalyzerCSubset, ArrayParamDecorationsAllDecayClean) {
    auto model = analyzeShipped("c-subset", {
        "int fa(int n, int p[static n]) { return p[0]; }\n"
        "int fb(int p[const 3])         { return p[0]; }\n"
        "int fc(int p[restrict])        { return p[0]; }\n"
        "int fd(int p[volatile 3])      { return p[0]; }\n"
        "int fg(int p[const static 3])  { return p[0]; }\n"
        "int main(void) { int x[3]; x[0]=1; x[1]=2; x[2]=3;\n"
        "  return fa(3,x)+fb(x)+fc(x)+fd(x)+fg(x); }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "every array-parameter decoration (static / const / volatile / restrict and "
           "combos) is legal in a parameter and decays to `int*`";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayParamQualifierNonParameter), 0u)
        << "no decoration on a PARAMETER may trip the non-parameter gate";
}

// VLA C4c (D-CSUBSET-VLA-PARAM-STAR): the bare unspecified-size `int a[*]` prototype-form
// VLA-parameter marker — LANDED via the distinct `arrayStarSuffix` grammar rule + the
// speculative-repeat-alt schema-compiler engine fix (grammar_schema_json.cpp). In a PARAMETER it
// decays to a bare pointer EXACTLY like `[]` (no error, no 0xE054); a NON-parameter `[*]` is a
// constraint violation → S_ArrayParamQualifierNonParameter (0xE054, the SAME paramDecay gate as a
// static/qualifier decoration). RED-ON-DISABLE: a regression that drops the `*` (mis-types the
// param as a plain `int`), fails to parse `[*]`, or fails to gate the non-param form flips this.
TEST(SemanticAnalyzerCSubset, ArrayParamStarFormDecaysCleanNonParamFailsLoud) {
    // PARAMETER `int a[*]` — decays to a bare pointer, compiles clean.
    auto param = analyzeShipped("c-subset", {
        "int f(int a[*]) { return a[0]; }\n"
        "int main(void) { int x[1]; x[0] = 7; return f(x); }\n",
    });
    EXPECT_FALSE(param.hasErrors())
        << "a PARAMETER `int a[*]` must decay to a bare pointer + compile clean";
    EXPECT_EQ(countCode(param.diagnostics(),
                        DiagnosticCode::S_ArrayParamQualifierNonParameter), 0u)
        << "a `[*]` on a PARAMETER must NOT trip the non-parameter gate";

    // NON-PARAMETER `int a[*];` (a local) — a constraint violation → 0xE054.
    auto local = analyzeShipped("c-subset", {
        "int main(void) { int a[*]; return 0; }\n",
    });
    EXPECT_TRUE(hasCode(local.diagnostics(),
                        DiagnosticCode::S_ArrayParamQualifierNonParameter))
        << "a `[*]` on a NON-parameter (local) is a constraint violation -> 0xE054, "
           "never silently accepted with the `*` dropped";
}

// A `[static N]` on a NON-parameter (a LOCAL) is a constraint violation — these decorations
// are legal ONLY in a function-parameter declarator (C 6.7.6.3p7). Fail loud with
// S_ArrayParamQualifierNonParameter (0xE054). RED-ON-DISABLE: without the paramDecay gate a
// local `[static 3]` would silently build an array with the decoration dropped.
TEST(SemanticAnalyzerCSubset, ArrayStaticOnLocalFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) { int a[static 3]; return a[0]; }\n",
    });
    EXPECT_TRUE(hasCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayParamQualifierNonParameter))
        << "a LOCAL `int a[static 3]` is a non-parameter constraint violation -> 0xE054";
}

// The same gate on a STRUCT FIELD (also a declarator-mode row with paramDecay=false).
TEST(SemanticAnalyzerCSubset, ArrayStaticOnStructFieldFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "struct S { int a[static 3]; };\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_TRUE(hasCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayParamQualifierNonParameter))
        << "a struct field `int a[static 3]` is a non-parameter constraint violation -> 0xE054";
}

// The same gate on an EXTERN object — the LEGACY `applyArraySuffix` path (externDecl is never
// a parameter). RED-ON-DISABLE: without the externDecl reject the widened suffix would
// silently DROP `static 5` (the fixed lengthChild index now points past it) -> a bogus
// incomplete array.
TEST(SemanticAnalyzerCSubset, ExternArrayStaticFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "extern int arr[static 5];\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_TRUE(hasCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayParamQualifierNonParameter))
        << "`extern int arr[static 5]` (never a parameter) -> 0xE054, not a silent drop";
}

// A GROUPED-inner declarator resets paramDecay (C 6.7.6.3p7 adjusts only the OUTERMOST dim),
// so a decoration on a grouped inner array is a non-parameter use -> 0xE054 (deliberately
// stricter). RED-ON-DISABLE: the paramDecay=false reset at the group recursion is what makes
// this fire; drop it and the inner `[static 3]` is wrongly accepted.
TEST(SemanticAnalyzerCSubset, ArrayStaticOnGroupedInnerParamFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "int f(int (*p[static 3]));\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_TRUE(hasCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayParamQualifierNonParameter))
        << "a `[static 3]` on a grouped-inner declarator is not the decaying outermost dim "
           "-> 0xE054 (the paramDecay reset)";
}

// VLA C4c REGRESSION GUARD (D-CSUBSET-VLA, audit MUST-FIX 1): `int a[*p]` — a LOCAL VLA sized
// by a DEREF `*p` — compiled BEFORE C4c and must STILL compile after the `[*]` (arrayStarSuffix)
// landing. The suffix repeat is speculative: the fixed 3-token `arrayStarSuffix` (`[ * ]`) probes
// FIRST, fails at the `]` position (it sees `p`, not `]`), and rolls back CLEANLY to
// `arrayDeclSuffix`, which parses `*p` as a normal VLA bound EXPRESSION. `*p` is an EXPRESSION
// node under arrayDeclSuffix, NOT the bare-`*` arrayStarSuffix, so it is NEITHER a `[*]` marker
// (no 0xE054) NOR read as absent. RED-ON-DISABLE: a non-speculative `arrayStarSuffix`-first
// dispatch (committing on the shared `[`), or a lost engine rollback, regresses this working VLA.
// The gate corpus has no `[*expr]`, so this is the ONLY guard.
TEST(SemanticAnalyzerCSubset, DerefSizedVlaStillCompiles) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "  volatile int vn = 3;\n"
        "  int n = vn;\n"
        "  int *p = &n;\n"
        "  int a[*p];\n"                 // a VLA sized by *p (== 3)
        "  a[0] = 7;\n"
        "  return a[0];\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "`int a[*p]` (a deref-sized VLA) must STILL compile — the `[*]`-vs-`[*expr]` "
           "speculation must parse `*p` as the bound, not the `*` decoration";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayParamQualifierNonParameter), 0u)
        << "a deref bound `*p` is an expression, NOT the `[*]` decoration — no 0xE054";
}

// VLA C4c (D-CSUBSET-VLA, code-audit IMPORTANT): the EMERGENT multi-dim `[*]` combinations now
// parse+fold (arrayStarSuffix rides the suffix repeat) — outside C4c's single-`[*]` scope. They
// must be CORRECT or FAIL LOUD, NEVER a silent stride: `int a[*][3]` is the OUTER star-modifier
// (decays exactly like `int a[][3]` → `int(*)[3]`, a FIXED inner stride — accepted, no VLA); an
// INNER `[*]` (`int a[n][*]`) yields a pointer to an UNSPECIFIED-size array whose fixed-array
// argument is a distinct interned type → `S_TypeMismatch` (S0003) fail-loud (NEVER a wrong/zero
// row stride). RED-ON-DISABLE: an inner `[*]` silently accepted with a bogus stride would flip
// the arg-compat reject.
TEST(SemanticAnalyzerCSubset, ArrayStarOuterDecaysInnerStarFailsLoud) {
    auto outer = analyzeShipped("c-subset", {
        "int f(int a[*][3]) { return a[1][2]; }\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_FALSE(outer.hasErrors())
        << "`int a[*][3]` = an outer star-modifier + fixed inner: decays to `int(*)[3]` "
           "(a fixed inner stride), accepted exactly like `int a[][3]`";
    auto inner = analyzeShipped("c-subset", {
        "int f(int n, int a[n][*]) { return a[0][0]; }\n"
        "int main(void) { int x[2][3] = {{1,0,0},{0,0,0}}; return f(2, x); }\n",
    });
    EXPECT_TRUE(inner.hasErrors())
        << "an INNER `[*]` (`int a[n][*]`) → ptr-to-unspecified-array: a fixed-array arg is a "
           "distinct interned type → S_TypeMismatch fail-loud, NEVER a silent stride";
}

// VLA C4c (D-CSUBSET-VLA, audit IMPORTANT 3): a multi-dim VLA parameter whose INNER dim
// carries a lenient `static` (`int a[n][static m]`) must locate the REAL inner bound `m` (the
// shared bound-locator skips the `static`), never mis-size or spuriously reject. The inner
// `static` is leniently accepted on a parameter (both dims carry paramDecay=true, so the gate
// does not fire), and `m` still types the inner dimension. RED-ON-DISABLE: a mis-located bound
// (reading `static` instead of `m`) would query the wrong node's type -> a spurious
// S_VlaSizeNotInteger.
TEST(SemanticAnalyzerCSubset, MultiDimParamInnerStaticSizesCorrectly) {
    auto model = analyzeShipped("c-subset", {
        "int f(int n, int m, int a[n][static m]);\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_VlaSizeNotInteger), 0u)
        << "the inner `[static m]` bound must resolve to `m` (integer), never the `static` "
           "token -> no spurious S_VlaSizeNotInteger";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayParamQualifierNonParameter), 0u)
        << "a lenient inner `[static m]` on a PARAMETER (paramDecay=true both dims) must NOT "
           "trip the non-parameter gate";
    EXPECT_FALSE(model.hasErrors())
        << "`int a[n][static m]` (a multi-dim VLA param with a lenient inner `static`) "
           "analyzes clean — the decoration is skipped, `m` sizes the inner dim";
}

// D-CSUBSET-VLA-PTR-INIT-FORM-TYPING boundary guard: the INITIALIZER form
// `int (*p)[n] = b;` is DEFERRED (the initializer node is pre-stamped decayed, defeating
// the init-compat derivation; C4a-local witnesses via the assignment form `p = b;`). It
// must FAIL LOUD at the semantic tier (S_TypeMismatch) — NOT silently accept. ★ This pin
// is the safety boundary for the deferral: a future PARTIAL fix that makes `= b`
// assignable WITHOUT also fixing the body-typing wrinkle would silently convert this safe
// reject into a wrong-STRIDE miscompile at the subscript. This test goes RED on exactly
// that dangerous partial change; when the init form PROPERLY lands it is flipped to
// accept + a runtime witness. (Assignment-form `int (*p)[n]; p = b; p[i][j]` RUNS today.)
TEST(SemanticAnalyzerCSubset, PtrToVlaInitFormDeferredStillFailsLoud) {
    auto model = analyzeShipped("c-subset", {
        "int main(void) {\n"
        "  volatile int vn = 3;\n"
        "  int n = vn;\n"
        "  int b[2][n];\n"
        "  int (*p)[n] = b;\n"          // the INIT form (deferred) — must reject, not run
        "  return p[1][0];\n"
        "}\n",
    });
    EXPECT_TRUE(model.hasErrors())
        << "the pointer-to-VLA INIT form `int (*p)[n] = b` is deferred and must fail loud "
           "(S_TypeMismatch) — never a silent accept that would mis-stride the subscript";
}

// ─── FC17.9(e) (D-CSUBSET-LONG-DOUBLE): the per-format long-double axis ──────
//
// `long double`'s REPRESENTATION is ABI-divergent per object format (64-bit
// IEEE on pe64/apple-arm64, x87 80-bit on SysV/darwin x86_64, binary128 on
// linux-arm64), so the c-subset typeSpecifiers row carries a
// coreByLongDoubleFormat map resolved against `analyze()`'s LongDoubleFormat
// axis: f64 → F64 (long double IS double — the full machinery serves it),
// x87-80 → F80, ieee128 → F128, and an UNDECLARED axis (None — wasm/spirv/
// direct-API) leaves the row UNREALIZED → the precise
// S_LongDoubleFormatUndeclared, NEVER a silently-guessed base core.

// (Symbol lookup reuses the file-wide `findSymbolNamed` helper above.)
namespace {
[[nodiscard]] SemanticModel analyzeWithLongDoubleAxis(
    std::initializer_list<std::string> sources, LongDoubleFormat axis) {
    auto cu = buildShippedUnit("c-subset", sources);
    assertNoBuilderErrors(*cu);
    return analyze(cu, DataModel::Lp64, std::nullopt, std::nullopt,
                   std::nullopt, std::nullopt, axis);
}
} // namespace

TEST(SemanticAnalyzerCSubset, LongDoubleResolvesPerAxis) {
    struct Row { LongDoubleFormat axis; TypeKind expected; };
    for (Row const row : {Row{LongDoubleFormat::F64, TypeKind::F64},
                          Row{LongDoubleFormat::X87_80, TypeKind::F80},
                          Row{LongDoubleFormat::Ieee128, TypeKind::F128}}) {
        auto model = analyzeWithLongDoubleAxis(
            {"int main(void) { long double x; }\n"}, row.axis);
        EXPECT_FALSE(model.hasErrors())
            << "axis " << static_cast<int>(row.axis)
            << ": a `long double` declaration must resolve";
        auto const* x = findSymbolNamed(model, "x");
        ASSERT_NE(x, nullptr);
        ASSERT_TRUE(x->type.valid());
        EXPECT_EQ(model.lattice().interner().kind(x->type), row.expected)
            << "axis " << static_cast<int>(row.axis);
    }
}

TEST(SemanticAnalyzerCSubset, LongDoubleUndeclaredAxisFailsLoud) {
    // Default analyze() = LongDoubleFormat::None (direct-API / wasm / spirv):
    // the row is UNREALIZED — the PRECISE 0xE056, not the generic S0011, and
    // NEVER a silent F64 bind (the base-core-fallback trap, IMPORTANT-4).
    auto model = analyzeShipped("c-subset", {
        "int main(void) { long double x; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_LongDoubleFormatUndeclared), 1u)
        << "a `long double` DECLARATION under an undeclared axis must emit "
           "S_LongDoubleFormatUndeclared (0xE056)";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidTypeSpecifierCombination), 0u)
        << "the combination is VALID C — the unrealized-row arm must fire "
           "BEFORE the invalid-combination miss";
    auto const* x = findSymbolNamed(model, "x");
    ASSERT_NE(x, nullptr);
    EXPECT_FALSE(x->type.valid())
        << "the symbol must stay UNTYPED — a valid TypeId here means the row "
           "silently base-core-resolved under an undeclared axis";
}

TEST(SemanticAnalyzerCSubset, LongDoubleIsDoubleOnF64Axis) {
    // The f64 axis (pe64 / apple-arm64): `long double` COLLAPSES to F64 —
    // assignment-compatible with `double` in BOTH directions (the LLP64
    // long==int identity-collapse precedent), so the whole double machinery
    // serves it with zero new codegen.
    auto model = analyzeWithLongDoubleAxis(
        {"int main(void) { long double x; double d; x = d; d = x; x = 1.5; }\n"},
        LongDoubleFormat::F64);
    EXPECT_FALSE(model.hasErrors())
        << "on the f64 axis `long double` IS double — both assignment "
           "directions must be clean";
}

TEST(SemanticAnalyzerCSubset, LongDoubleLiteralTypesPerAxis) {
    // C 6.4.4.2: the l/L float suffix types `long double` — resolved through
    // the SAME axis map the typeSpecifiers row carries. On x87-80 a `20.0L`
    // initializer binds an F80 `long double` cleanly; the sibling `20L`
    // INTEGER literal stays `long` (the CRITICAL-1 suffix-shape pin, semantic
    // tier: it must remain a valid ARRAY DIMENSION, which no float can be).
    auto ld = analyzeWithLongDoubleAxis(
        {"int main(void) { long double x = 20.0L; }\n"},
        LongDoubleFormat::X87_80);
    EXPECT_FALSE(ld.hasErrors())
        << "`long double x = 20.0L;` on the x87-80 axis: the literal types "
           "F80 and binds the F80 declaration cleanly";

    auto intL = analyzeWithLongDoubleAxis(
        {"int main(void) { int a[20L]; return sizeof a ? 0 : 1; }\n"},
        LongDoubleFormat::X87_80);
    EXPECT_FALSE(intL.hasErrors())
        << "`20L` must stay an INTEGER `long` (a constant array dimension) — "
           "an error here means the l-suffix float rule swallowed it";

    // Undeclared axis: the long-double LITERAL is as unknowable as the
    // declaration — the same precise 0xE056 (never a silent F64 typing).
    auto none = analyzeShipped("c-subset", {
        "int main(void) { double d = 20.0L; }\n",
    });
    EXPECT_EQ(countCode(none.diagnostics(),
                        DiagnosticCode::S_LongDoubleFormatUndeclared), 1u)
        << "a `20.0L` literal under an undeclared axis must emit 0xE056";
}

TEST(SemanticAnalyzerCSubset, LongDoubleUsualArithmeticConversionOutranksDouble) {
    // C 6.3.1.8: long double outranks double (floatRank F80=4 > F64=3). On a
    // WALLED axis the arm is observable: `x + d` types F80, so it binds an
    // F80 lhs cleanly and REJECTS an F64 lhs (were the result F64, the two
    // expectations would invert — a strict both-ways pin of the rank order).
    auto good = analyzeWithLongDoubleAxis(
        {"int main(void) { long double x; double d; long double r; r = x + d; }\n"},
        LongDoubleFormat::X87_80);
    EXPECT_FALSE(good.hasErrors())
        << "`long double + double` must type long double (F80) — assignable "
           "into a long double lhs";

    auto bad = analyzeWithLongDoubleAxis(
        {"int main(void) { long double x; double d; double r; r = x + d; }\n"},
        LongDoubleFormat::X87_80);
    EXPECT_TRUE(bad.hasErrors())
        << "`long double + double` into a DOUBLE lhs is a narrowing float "
           "assignment (F80 -> F64) — must reject, proving the UAC result is "
           "F80, not F64";
}

TEST(SemanticAnalyzerCSubset, LongDoubleConstexprFoldRefusedOnWalledAxis) {
    // IMPORTANT-5 (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION): the const-eval
    // fold gate. F80/F128 are NOT host-backed (floatKindInfo.hostBacked ==
    // false) — folding `20.0L + 22.0L` at the host's binary64 would bake a
    // silently-rounded constant for any value needing >53 mantissa bits. The
    // gate refuses at applyBinaryFloat, so the constexpr initializer is NOT a
    // compile-time constant on a walled axis (loud), while the SAME source on
    // the f64 axis folds exactly (long double IS binary64 there).
    auto walled = analyzeWithLongDoubleAxis(
        {"int main(void) { constexpr long double k = 20.0L + 22.0L; }\n"},
        LongDoubleFormat::X87_80);
    EXPECT_EQ(countCode(walled.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 1u)
        << "F80 constexpr arithmetic must REFUSE the host-double fold "
           "(hostBacked==false) — a clean analysis here means the fold gate "
           "is bypassed and a binary64-rounded constant was baked";

    auto f64 = analyzeWithLongDoubleAxis(
        {"int main(void) { constexpr long double k = 20.0L + 22.0L; }\n"},
        LongDoubleFormat::F64);
    EXPECT_FALSE(f64.hasErrors())
        << "on the f64 axis the SAME fold is exact (long double IS binary64) "
           "— must fold clean";
}
