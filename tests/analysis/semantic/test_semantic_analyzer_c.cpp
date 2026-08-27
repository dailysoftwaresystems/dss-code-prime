// SE2 acceptance: c language end-to-end via the same
// SchemaDrivenSemantics engine — proves zero per-language C++ is needed
// to add a new language with built-in types, lexical block scopes, and
// typed literals.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_visitor.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "analysis/semantic/inline_asm_facts.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

using namespace dss;
using namespace dss::sem_test;

// `int x;` inside a function body parses through varDecl → varDeclHead,
// which the c `semantics` block declares as a Variable decl
// (name=1, type=0). Should mint one symbol typed I32. The top-level
// `int main()` ALSO mints a symbol — via the c `topLevelDecl`
// declaration with its `kindByChild` discriminator (whenRule =
// funcDefTail → kind=Function). So we expect two symbols total: `main`
// (Function) and `x` (Variable); we find `x` by name.
TEST(SemanticAnalyzerC, FunctionLocalIntDeclTypedAsI32) {
    auto cu = buildShippedUnit("c", {
        "int main() { int x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    // main (function) + x (variable) + the 3 FC12a-core builtin TYPES
    // (`__va_list_tag` + `va_list` + the `__builtin_va_list` alias,
    // D-CSUBSET-BUILTIN-VA-LIST-TYPE-NAME) injected into every c CU's builtin
    // scope (D-FC12A-VARIADIC-CALLEE — gated on the schema declaring `vaArgRule`) + the
    // 6 intrinsic builtin FUNCTIONS (SE6 builtinFunctions, minted into the same
    // CU-wide builtins scope): TF-C95 `__sync_synchronize` (D-CSUBSET-ATOMIC-FENCE)
    // + c103 `__umulh` (D-CSUBSET-INTRINSIC-UMULH) + c104
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
    // D-CSUBSET-INTRINSIC-BSWAP: + the 6 byte-swap builtins (`_byteswap_ushort`/
    // `_byteswap_ulong`/`_byteswap_uint64` + their GCC spellings
    // `__builtin_bswap16`/`__builtin_bswap32`/`__builtin_bswap64` — six NAMES over
    // ONE `lowering: "bswap"` verb, always-injected like every other builtin).
    ASSERT_EQ(model.symbols().size() - 1, 87u)
        << "main + x + __va_list_tag + va_list + __builtin_va_list + __umulh + "
           "_InterlockedCompareExchange + _ReadWriteBarrier + __sync_synchronize + "
           "_exception_code + _exception_info + the 6 __builtin bit-count "
           "intrinsics + the 56 __builtin_stdc_* <stdbit.h> intrinsics + "
           "atomic_load_explicit + atomic_store_explicit + the 4 __builtin_complex/"
           "creal/cimag/conj complex builtins + the 6 byte-swap builtins "
           "(_byteswap_ushort/_byteswap_ulong/_byteswap_uint64 + "
           "__builtin_bswap16/32/64) + __func__ + __FUNCTION__";
    SymbolRecord const* xRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "x") xRec = &model.symbols()[i];
    }
    ASSERT_NE(xRec, nullptr);
    ASSERT_TRUE(xRec->type.valid()) << "the int builtin must resolve to a TypeId";
    EXPECT_EQ(model.lattice().interner().kind(xRec->type), TypeKind::I32);
}

// D-CSUBSET-EXTERN-AGGREGATE-TYPE (TF arc C6, SQLite testfixture): a file-scope
// `extern` declaration whose type-specifier is a struct/union/enum aggregate now
// PARSES (typeBase gained the structSpec/unionSpec/enumSpec arms — the extern path
// via externDecl→typeRef→typeBase previously omitted them, so `extern struct Foo g;`
// P0009'd while `static struct`/plain/typedef all worked) AND resolves the extern
// symbol to the aggregate TypeId, exactly like a `static struct Foo g;` (the type
// resolver dispatches on the composite node's own rule, not on externDecl — zero
// semantic change). This is the dominant sqlite-testfixture compile blocker
// (sqliteInt.h `extern SQLITE_WSD struct Sqlite3Config sqlite3Config;`).
// RED-ON-DISABLE: revert typeBase's alt list to {typeSpecifierSeq, Identifier,
// typeofSpecifier} and `assertNoBuilderErrors` fails on the P0009 parse error for
// every `extern struct/union/enum` line below. The enum form is covered ONLY here
// (its GLOBAL codegen is a separate pre-existing gap, D-CSUBSET-ENUM-GLOBAL-CODEGEN,
// so the runtime example extern_aggregate omits it); struct/union/const/pointer are
// additionally witnessed end-to-end by that example.
TEST(SemanticAnalyzerC, ExternAggregateSpecifiersParse) {
    auto cu = buildShippedUnit("c", {
        "struct S { int v; };\n"
        "union U { int a; };\n"
        "enum E { EV = 1 };\n"
        "extern struct S gS;\n"          // extern + struct tag  (was P0009 "got struct")
        "extern union U gU;\n"           // extern + union tag   (was P0009 "got union")
        "extern enum E gE;\n"            // extern + enum tag    (was P0009 "got enum")
        "extern const struct S gC;\n"    // extern + const struct
        "extern struct S *gP;\n"         // extern + pointer-to-struct
        "extern struct SB { int w; } gB;\n"  // extern + struct DEFINITION (inline body form)
    });
    assertNoBuilderErrors(*cu);          // red-on-disable hook: every extern line above must parse
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto rec = [&](std::string_view n) -> SymbolRecord const* {
        for (std::size_t i = 1; i < model.symbols().size(); ++i)
            if (model.symbols()[i].name == n) return &model.symbols()[i];
        return nullptr;
    };
    auto const& in = model.lattice().interner();
    for (char const* n : {"gS", "gU", "gE", "gC", "gP", "gB"}) {
        auto const* r = rec(n);
        ASSERT_NE(r, nullptr) << "extern aggregate symbol '" << n << "' was not minted";
        ASSERT_TRUE(r->type.valid()) << "extern aggregate symbol '" << n << "' has no resolved type";
    }
    EXPECT_EQ(in.kind(rec("gS")->type), TypeKind::Struct);
    EXPECT_EQ(in.kind(rec("gU")->type), TypeKind::Union);
    EXPECT_EQ(in.kind(rec("gE")->type), TypeKind::Enum);
    EXPECT_EQ(in.kind(rec("gC")->type), TypeKind::Struct);  // const is a qualifier bit; base kind stays Struct
    EXPECT_EQ(in.kind(rec("gP")->type), TypeKind::Ptr);     // pointer-to-struct decays to Ptr
    EXPECT_EQ(in.kind(rec("gB")->type), TypeKind::Struct);  // inline struct-definition-in-extern (body form)
}

// c23 D-CSUBSET-EXTERN-MULTI-DECLARATOR: a MULTI-declarator `extern` mints ONE
// nonDefiningDeclaration symbol PER declarator, each with its OWN per-declarator
// pointer/array suffix folded onto the shared head base type — `extern int a, b;`
// mints a AND b (both int); `extern int *p, arr[3];` mints p (int*) and arr
// (int[3]). RED-ON-DISABLE: reverting externDecl to the single-declarator spine
// P0009's on the comma, so `assertNoBuilderErrors` fails (the decl won't parse);
// a shared-head star (the retired `typeRef` spine) would type `arr`/`b` as Ptr too.
TEST(SemanticAnalyzerC, ExternMultiDeclaratorMintsPerDeclaratorSymbols) {
    auto cu = buildShippedUnit("c", {
        "extern int a, b;\n"          // two objects, ONE extern declaration
        "extern int *p, arr[3];\n"    // per-declarator pointer (p) + array (arr)
    });
    assertNoBuilderErrors(*cu);       // red-on-disable: the multi-declarator externs must parse
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto rec = [&](std::string_view n) -> SymbolRecord const* {
        for (std::size_t i = 1; i < model.symbols().size(); ++i)
            if (model.symbols()[i].name == n) return &model.symbols()[i];
        return nullptr;
    };
    auto const& in = model.lattice().interner();
    for (char const* n : {"a", "b", "p", "arr"}) {
        auto const* r = rec(n);
        ASSERT_NE(r, nullptr) << "multi-declarator extern symbol '" << n
                              << "' was not minted (one symbol per declarator)";
        ASSERT_TRUE(r->type.valid()) << "extern symbol '" << n << "' has no type";
        EXPECT_TRUE(r->isExternDeclaration)
            << "extern symbol '" << n << "' must be a non-defining declaration";
    }
    // Per-declarator TYPES: the pointer/array suffix binds to its OWN declarator.
    EXPECT_EQ(in.kind(rec("a")->type), TypeKind::I32);
    EXPECT_EQ(in.kind(rec("b")->type), TypeKind::I32);   // NOT a pointer (no shared-head star)
    EXPECT_EQ(in.kind(rec("p")->type), TypeKind::Ptr);   // `*p` — a pointer
    EXPECT_EQ(in.kind(rec("arr")->type), TypeKind::Array); // `arr[3]` — an array, not int/ptr
}

// D-CSUBSET-EXTERN-FN-DEFINITION (§B 2026-07-21): an `extern` on a FUNCTION
// DEFINITION (`extern int f(int){…}`) mints a DEFINING Function (a body present),
// NOT a non-defining declaration — despite externDecl's `nonDefiningDeclaration`
// default. The kindByChild body-block discriminator upgrades effectiveKind to
// Function, and the engine suppresses isExtern for a Function-kind declarator. It
// contrasts with a bare `extern` PROTOTYPE (Variable-kind, isProto, isExtern — a
// non-defining declaration) and mirrors a `static` DEFINITION (also Function-kind,
// defining — the linkage difference is a MIR-tier concern, pinned separately).
// RED-ON-DISABLE: revert externDeclTail's block arm -> `extern int efd(int x){…}`
// P0009s (assertNoBuilderErrors fails); revert the isExtern-Function guard -> efd
// becomes isExternDeclaration (this test's EXPECT_FALSE reds).
TEST(SemanticAnalyzerC, ExternFunctionDefinitionMintsDefiningFunction) {
    auto cu = buildShippedUnit("c", {
        "extern int efd(int x){ return x + 1; }\n"   // extern DEFINITION (a body)
        "extern int eproto(void);\n"                 // extern PROTOTYPE (no body)
        "static int sfd(int x){ return x + 1; }\n"   // static DEFINITION (contrast)
    });
    assertNoBuilderErrors(*cu);   // red-on-disable: the extern DEFINITION must parse
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto rec = [&](std::string_view n) -> SymbolRecord const* {
        for (std::size_t i = 1; i < model.symbols().size(); ++i)
            if (model.symbols()[i].name == n) return &model.symbols()[i];
        return nullptr;
    };
    // The extern function DEFINITION: a DEFINING Function (a body present), NOT a
    // non-defining declaration and NOT a syntactic prototype.
    auto const* efd = rec("efd");
    ASSERT_NE(efd, nullptr) << "the extern function definition must mint a symbol";
    EXPECT_EQ(efd->kind, DeclarationKind::Function)
        << "`extern int efd(int){…}` is a function DEFINITION (Function-kind)";
    EXPECT_FALSE(efd->isExternDeclaration)
        << "a definition is DEFINING — never a non-defining extern declaration";
    EXPECT_FALSE(efd->isProtoDeclaration)
        << "a definition (a body) is not a bare prototype";
    // The bare extern PROTOTYPE: a NON-DEFINING declaration. Pass-1.5 upgrades a
    // proto's kind to Function (D-CSUBSET-FN-PROTOTYPE) exactly as for a topLevel
    // proto, so BOTH efd and eproto are Function-kind — the def-vs-declaration
    // distinction is isProto/isExtern (below), NOT the kind. This is the contrast
    // that matters: efd is defining (isExtern=false), eproto is not (isExtern=true).
    auto const* eproto = rec("eproto");
    ASSERT_NE(eproto, nullptr);
    EXPECT_TRUE(eproto->isProtoDeclaration)
        << "the bare `extern int eproto(void);` is a syntactic proto";
    EXPECT_TRUE(eproto->isExternDeclaration)
        << "the bare extern prototype is a NON-defining declaration (unlike the "
           "extern DEFINITION efd, which is defining)";
    // The `static` DEFINITION: also a DEFINING Function (contrast — same defining
    // shape at the semantic tier; the internal-vs-external linkage lives at MIR).
    auto const* sfd = rec("sfd");
    ASSERT_NE(sfd, nullptr);
    EXPECT_EQ(sfd->kind, DeclarationKind::Function);
    EXPECT_FALSE(sfd->isExternDeclaration);
}

// C99 _Complex (D-CSUBSET-COMPLEX §6.2.5): `double _Complex z;` resolves the
// `[Complex, Double]` type-specifier multiset to a Complex over F64 (via the
// `complex:true` typeSpecifiers row + the interner.complex wrap). `float _Complex`
// → Complex over F32.
TEST(SemanticAnalyzerC, ComplexDeclTypedAsComplex) {
    auto cu = buildShippedUnit("c", {
        "int main() { double _Complex z; float _Complex w; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ComplexIntAndImaginaryFailLoud) {
    auto ci = analyzeShipped("c", {"int main() { _Complex int y; }\n"});
    EXPECT_TRUE(ci.hasErrors());
    EXPECT_GT(countCode(ci.diagnostics(),
                        DiagnosticCode::S_InvalidTypeSpecifierCombination), 0u)
        << "`_Complex int` is not a valid type-specifier multiset — must fail loud";
    auto im = analyzeShipped("c", {"int main() { _Imaginary x; }\n"});
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
TEST(SemanticAnalyzerC, ComplexConstexprInitializerRefusesToFold) {
    // (a) a complex-constructing builtin feeding a scalar accessor: not foldable.
    auto viaBuiltin = analyzeShipped("c", {
        "int main(void) { constexpr double r = "
        "__builtin_creal(__builtin_complex(40.0, 2.0)); }\n"});
    EXPECT_EQ(countCode(viaBuiltin.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 1u)
        << "a complex construction in a constexpr initializer must REFUSE the "
           "fold — a clean analysis means a complex value was silently baked";
    // (b) an explicit real->complex->real cast chain: the complex cast TARGET
    // refuses at the const-eval cast classification.
    auto viaCast = analyzeShipped("c", {
        "int main(void) { constexpr double d = (double)(double _Complex)2.0; }\n"});
    EXPECT_EQ(countCode(viaCast.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 1u)
        << "a cast through a complex type in a constexpr initializer must "
           "refuse to fold";
    // Negative control: WITHOUT constexpr the same expressions are legal RUNTIME
    // constructions (the refusal above is the fold gate, not a reject of complex).
    auto runtime = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ArrayDeclaratorTypedAsArray) {
    auto cu = buildShippedUnit("c", {
        "int main() { int a[10]; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, NonConstantArrayLengthEmitsDiagnostic) {
    auto cu = buildShippedUnit("c", {
        "int n;\n"
        "int g[n];\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 1u);
}

// SE-arrays: an empty-bracket declarator (`int a[]`) has no length — a DIFFERENT
// path from `[n]` (the length node lands on the `]` token, not an identifier).
// Must also fail loud.
TEST(SemanticAnalyzerC, EmptyArrayLengthEmitsDiagnostic) {
    auto cu = buildShippedUnit("c", { "int main() { int a[]; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 1u);
}

// ⚠⚠ REWRITTEN BY INLINE-ASM P5, AND THE OLD PIN IS KEPT AS PROSE BECAUSE ITS
// PREMISE EXPIRED RATHER THAN BEING WRONG. Until P5 these two tests asserted
// `__asm__("nop")` and `__asm__("  ")` each cost one
// S_InlineAsmNonEmptyTemplate: cycle-1 could not EMIT a non-empty template, so
// refusing it was the only alternative to dropping the instructions. P5 carries
// the template into the HIR descriptor, so a non-empty template is now the
// ORDINARY case and refusing it would be the divergence — every reference
// compiler accepts `__asm__("nop")`.
//
// ★ WHAT 0xE057 STILL MEANS, and why the code did not become dead: its
// surviving arm is "no template child of the configured shape was found", a
// CONFIG/GRAMMAR disagreement rather than a statement about DSS's maturity.
// That arm is unreachable from a well-formed shipped grammar, which is exactly
// why it must stay — it is the fail-loud for a config edit that breaks the
// locator (`decodeAdjacentStringBodies` returns "" for a mis-picked node, so a
// silent locator failure would look like an EMPTY template and pass).
TEST(SemanticAnalyzerC, InlineAsmNonEmptyTemplateIsNoLongerRefused) {
    for (char const* src : {"int main(void){ __asm__(\"nop\"); return 0; }\n",
                            "int main(void){ __asm__(\"  \"); return 0; }\n"}) {
        auto cu = buildShippedUnit("c", {src});
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_InlineAsmNonEmptyTemplate), 0u)
            << src << ": gcc and clang both accept this; refusing it is the "
                      "divergence now";
        EXPECT_FALSE(model.hasErrors()) << src;
    }
}

// The strictly-empty template (with OR without `volatile`) is accepted — no reject, no
// errors. volatile is inert for the empty form (a no-output asm is implicitly volatile;
// GCC 6.47.2.1), so both spellings lower to the same barrier.
TEST(SemanticAnalyzerC, InlineAsmEmptyTemplateAccepted) {
    auto cu = buildShippedUnit("c", {
        "int main(void){ __asm__ volatile(\"\"); __asm__(\"\"); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InlineAsmNonEmptyTemplate), 0u);
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
}

// SE-arrays: a non-decimal length exercises the shared decodeInteger through the
// NEW semantic consumer — `0x10` must decode to 16 (radix handling), not be
// rejected as non-constant.
TEST(SemanticAnalyzerC, HexArrayLengthDecodes) {
    auto cu = buildShippedUnit("c", { "int main() { int a[0x10]; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, OutOfRangeArrayLengthEmitsDiagnostic) {
    auto cu = buildShippedUnit("c", {
        "int main() { int a[0xFFFFFFFFFFFFFFFF]; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ArraySizeInferredFromStringInit) {
    auto cu = buildShippedUnit("c", { "char x[] = \"abc\";\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ArraySizeInferredFromBraceInit) {
    auto cu = buildShippedUnit("c", { "int a[] = {1, 2, 3};\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ArraySizeInferredFromInitLocal) {
    auto cu = buildShippedUnit("c",
                               { "int main(void){ int a[] = {10, 20, 30}; return a[2]; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ExplicitArraySizeUnchangedByInference) {
    auto cu = buildShippedUnit("c",
                               { "int a[3] = {1, 2, 3}; char x[8] = \"abc\";\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, WideStringLiteralElementCorePerOpener) {
    struct Case { char const* src; TypeKind core; std::int64_t len; };
    for (auto const& tc : {Case{"void f(){ u\"AB\"; }",  TypeKind::U16, 3},
                           Case{"void f(){ U\"AB\"; }",  TypeKind::U32, 3},
                           Case{"void f(){ u8\"AB\"; }", TypeKind::U8,  3},
                           Case{"void f(){ \"AB\"; }",   TypeKind::Char, 3}}) {
        auto cu = buildShippedUnit("c", { tc.src });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, WideStringBmpMultibyteCodeUnitCount) {
    auto cu = buildShippedUnit("c", { "void f(){ u\"\xe2\x82\xac\"; }" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, WideCharLiteralWidthIsFormatKeyed) {
    // Default (activeFormat=nullopt) → I32.
    {
        auto cu = buildShippedUnit("c", { "void f(){ L\"AB\"; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
        auto cu = buildShippedUnit("c", { "void f(){ L\"AB\"; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Llp64, std::nullopt, std::nullopt,
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
TEST(SemanticAnalyzerC, WideCharLiteralScalarCorePerPrefix) {
    struct Case { char const* src; TypeKind core; };
    for (auto const& tc : {Case{"void f(){ 'x'; }",   TypeKind::I32},
                           Case{"void f(){ u'A'; }",  TypeKind::U16},
                           Case{"void f(){ U'A'; }",  TypeKind::U32},
                           Case{"void f(){ u8'A'; }", TypeKind::U8}}) {
        auto cu = buildShippedUnit("c", { tc.src });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, WideCharConstantWidthIsFormatKeyed) {
    // Default (activeFormat=nullopt) → I32.
    {
        auto cu = buildShippedUnit("c", { "void f(){ L'x'; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        ASSERT_FALSE(model.hasErrors());
        auto const& ti = model.lattice().interner();
        TypeId const ty = firstCharLiteralType(model, *cu);
        ASSERT_TRUE(ty.valid());
        EXPECT_EQ(ti.kind(ty), TypeKind::I32) << "wchar_t defaults to the POSIX i32 width";
    }
    // PE format → U16.
    {
        auto cu = buildShippedUnit("c", { "void f(){ L'x'; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Llp64, std::nullopt, std::nullopt,
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
TEST(SemanticAnalyzerC, BadWideCharConstantLeavesBodyTokenUntyped) {
    // u8'β' — U+03B2 exceeds the single-UTF-8-unit range (0x7F).
    {
        auto cu = buildShippedUnit("c", { "void f(){ u8'\xce\xb2'; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        TypeId const ty = firstCharLiteralType(model, *cu);
        EXPECT_FALSE(ty.valid())
            << "an out-of-range u8 char must be left untyped so sizeof fails loud";
    }
    // L'😀' under PE (U16) → astral, unrepresentable → untyped.
    {
        auto cu = buildShippedUnit("c", { "void f(){ L'\xf0\x9f\x98\x80'; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Llp64, std::nullopt, std::nullopt,
                             ObjectFormatKind::Pe);
        TypeId const ty = firstCharLiteralType(model, *cu);
        EXPECT_FALSE(ty.valid())
            << "an astral L' char under pe (u16 wchar_t) must be left untyped";
    }
    // L'😀' under the default format (I32 wchar_t) → representable → typed I32.
    {
        auto cu = buildShippedUnit("c", { "void f(){ L'\xf0\x9f\x98\x80'; }" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, EmptyArrayNoInitNotSized) {
    auto cu = buildShippedUnit("c", { "int main(void){ int a[]; return 0; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ArraySizeInferenceEmptyBraceFailsLoud) {
    auto cu = buildShippedUnit("c", { "int a[] = {};\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, PointerDeclaratorTypedAsPtr) {
    auto cu = buildShippedUnit("c", {
        "void f() { int *p; int **pp; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, VoidStarDeclaratorTypedAsPtrVoid) {
    auto cu = buildShippedUnit("c", { "void f() { void *p; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
// must accept without diagnostic (C-standard §6.3.2.3 — c
// declares both directions implicit in pointerConversions).
TEST(SemanticAnalyzerC, CharStarToVoidStarArgImplicit) {
    auto cu = buildShippedUnit("c", {
        "extern int handler(void* p);\n"
        "int main() {\n"
        "    char* s;\n"
        "    return handler(s);\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    // Strict countCode pin (replaces an earlier any-bool sawMismatch
    // loop that would have silently passed a wrong-code regression
    // — e.g., an S_ReturnTypeMismatch firing in place of S_TypeMismatch
    // would have masked the assertion).
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "char* → void* must be implicit in c "
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
TEST(SemanticAnalyzerC, MultiParamCallAddressOfArgsNoStaleParamSpan) {
    // The callee params are `void*` (NOT `int*`) — this is load-bearing for the
    // red-on-disable. The bug needs the arg's `subtreeType()` to intern a FRESH
    // type mid-loop: `&x` is `int*`, which is NOT already interned (the params are
    // `void*`), so checking it materializes pointer<int> and mutates the pool —
    // exactly memcpy's `void*` params + `&b`/`&a` `int*` args. (An `int*`-param
    // version does NOT trip it: `&x` dedups against the param's pointer<int>, no
    // mutation.) `int*` → `void*` is implicit in c, so the call is
    // well-typed; WITHOUT the owned-copy fix this analyze() aborts (guard) on Debug.
    auto cu = buildShippedUnit("c", {
        "void multi(void* a, void* b, int n);\n"
        "void f(void) {\n"
        "    int x;\n"
        "    int y;\n"
        "    multi(&x, &y, 4);\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    // The call is well-typed (int*→void* implicit, int→int): no mismatch, no abort.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArgCountMismatch), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// D-LANG-POINTER-VOID-CONVERT: the reverse direction (`void*` →
// `char*`) is also implicit under C semantics (c declares
// `implicitFromVoidPtr: true`) — C++ would forbid this without
// an explicit cast.
TEST(SemanticAnalyzerC, VoidStarToCharStarArgImplicit) {
    auto cu = buildShippedUnit("c", {
        "extern int handler(const char* s);\n"
        "extern void* alloc(int n);\n"
        "int main() {\n"
        "    return handler(alloc(16));\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "void* → const char* must be implicit in c "
           "(implicitFromVoidPtr: true). When C++ frontend lands, "
           "it would declare implicitFromVoidPtr: false and this "
           "direction would require an explicit cast.";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArgCountMismatch), 0u);
}

// D-LANG-POINTER-VOID-CONVERT negative pin: distinct typed pointers
// remain mismatch under c (only `void*` ↔ `T*` is implicit;
// `int*` → `char*` requires an explicit cast even in C).
TEST(SemanticAnalyzerC, DistinctTypedPointersRemainMismatch) {
    auto cu = buildShippedUnit("c", {
        "extern int handler(int* p);\n"
        "int main() {\n"
        "    char* s;\n"
        "    return handler(s);\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    // Pin EXACTLY ONE S_TypeMismatch (not duplicate cascade) AND
    // zero adjacent mismatch codes — replaces the loose any-bool
    // sawMismatch loop that would have admitted unrelated mismatch
    // codes (S_ReturnTypeMismatch / S_ArgCountMismatch) as satisfying
    // the assertion.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "char* → int* must NOT be implicit even in c — "
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
TEST(SemanticAnalyzerC, VoidStarReturnFromTypedPtrImplicit) {
    auto cu = buildShippedUnit("c", {
        "void* f(int* p) { return p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u)
        << "int* → void* via return must be implicit in c "
           "(implicitToVoidPtr: true)";
}

TEST(SemanticAnalyzerC, TypedPtrReturnFromVoidStarImplicit) {
    auto cu = buildShippedUnit("c", {
        "int* f(void* p) { return p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u)
        << "void* → int* via return must be implicit in c "
           "(implicitFromVoidPtr: true). C++ would forbid.";
}

TEST(SemanticAnalyzerC, DistinctTypedReturnRemainsMismatch) {
    auto cu = buildShippedUnit("c", {
        "int* f(char* p) { return p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 1u)
        << "char* → int* via return must NOT be implicit even in "
           "c (only void* gets the universal-pointer pass).";
}

// D-CSUBSET-POINTER-DIFF-ARRAY-DECAY: `pointer - arrayName` (C 6.5.6p9 + 6.3.2.1p3) —
// the array operand decays to Ptr<elem> FIRST, so the result is ptrdiff_t (an INTEGER),
// not a pointer. Used UNCAST in an integer context (the FTS5 `zOut - aBuf` shape, a
// token length passed as an int), it must be admitted. The semantic type-oracle
// (subtreeType/combineBinary) previously required BOTH operands already Ptr and never
// decayed an Array → `z - a` typed Ptr<char> → the int init failed isAssignable →
// S_TypeMismatch. (The pointer_minus_array example only ever CAST the result
// `(int)(t - arr)`, so the explicit cast masked this semantic gap.) Now the semantic arm
// decays the array first, mirroring the HIR combineBinary c65 that already lowers it.
// RED-ON-DISABLE: revert the semantic pointer-sub array-decay → the two UNCAST sites
// (`z - a`, `a - z`) type Ptr/Array in an int context → this count becomes 2.
TEST(SemanticAnalyzerC, PointerMinusArrayTypesAsPointerDifferenceInt) {
    auto cu = buildShippedUnit("c", {
        "int f(void){ char a[32]; char *z = a + 5;\n"
        "  int p = z - a;\n"          // ptr - array   (uncast, integer context)
        "  int q = a - z;\n"          // array - ptr
        "  int r = (int)(z - a);\n"   // the CAST control — clean either way
        "  return p + q + r; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "ptr-array and array-ptr subtraction must type as the ptrdiff int (the array "
           "decays first), not a pointer → S_TypeMismatch, in an uncast int context";
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
// conversion in c, so that pair is no longer a mismatch; a distinct-typed-
// pointer pair is the stable always-rejected case that still exercises the
// assignment-statement isAssignable path.)
// RED-ON-DISABLE: remove the assignment-statement isAssignable arm (restore the
// bypass) -> the assignment is silently accepted, this count drops to 0.
TEST(SemanticAnalyzerC, AssignStmtIntFromIncompatiblePointerFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "int sink(char* q) { int* p; p = q; return *p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
// admitted conversion in c [D-CSUBSET-INT-FLOAT-CONVERSION].)
// RED-ON-DISABLE: with the bypass restored only the INIT fires -> count is 1.
TEST(SemanticAnalyzerC, AssignStmtAndInitRejectIncompatibleIdentically) {
    auto cu = buildShippedUnit("c", {
        "int sink(char* q) { int* p = q; p = q; return *p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 2u)
        << "the init site AND the assignment-statement site must each reject the "
           "int* <- char* pair — two positioned S_TypeMismatch, not one";
}

// (c) A VALID assignment statement stays byte-identically clean: int <- int,
// pointer <- null-constant, and a cross-signedness assignment (the c
// `intCrossSignednessConverts` gate is ON) all pass with ZERO diagnostics. The
// new arm must not over-reject any conversion the four checked sites admit.
TEST(SemanticAnalyzerC, ValidAssignStmtsRemainClean) {
    auto cu = buildShippedUnit("c", {
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
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ValidLvalueStoreAssignStmtsRemainClean) {
    auto cu = buildShippedUnit("c", {
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
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
// spurious S_TypeMismatch here. (c does not yet LOWER compound-assign, but
// the SEMANTIC tier must not mis-reject it.)
TEST(SemanticAnalyzerC, CompoundAssignStmtNotCheckedAsPlainAssign) {
    auto cu = buildShippedUnit("c", {
        "int main(void) { int x; int y; y = 1; x = 0; x += y; return x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "a compound assignment (`x += y`) must not run the plain-assignment "
           "assignability check — only the plain `=` operator is checked";
}

// D-LANG-NULL-POINTER-CONSTANT (step 13.3, 2026-06-02): per C §6.3.2.3.3,
// the integer literal `0` is a null pointer constant — convertible to
// ANY pointer type without a cast. c declares
// `nullPointerConstantFromIntegerZero: true` in its `pointerConversions`
// block. These tests pin all three `isAssignable` call sites
// (call-arg, return, init) AND a strict-reject case for non-zero
// integer literals.
TEST(SemanticAnalyzerC, NullPointerConstantAdmitsAsVoidStarArg) {
    auto cu = buildShippedUnit("c", {
        "extern void f(void* p);\n"
        "int main() { f(0); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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

TEST(SemanticAnalyzerC, NullPointerConstantAdmitsAsTypedPointerArg) {
    auto cu = buildShippedUnit("c", {
        "extern void f(int* p);\n"
        "int main() { f(0); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    // C §6.3.2.3.3: NULL pointer constant converts to ANY pointer type
    // (not just void*) without a cast. F5 audit fix: pair countCode
    // with !hasErrors so wrong-code regressions can't silently pass.
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "literal 0 must convert to int* without an explicit cast";
}

TEST(SemanticAnalyzerC, NonZeroIntegerLiteralRejectsAsPointerArg) {
    auto cu = buildShippedUnit("c", {
        "extern void f(void* p);\n"
        "int main() { f(1); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    // Negative pin: ONLY the literal `0` admits as null pointer
    // constant — `1` (or any non-zero int) must NOT silently convert.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "non-zero int literal must NOT be admitted as a null "
           "pointer constant — only value-0 qualifies per C "
           "§6.3.2.3.3";
}

TEST(SemanticAnalyzerC, NullPointerConstantAdmitsAsReturn) {
    auto cu = buildShippedUnit("c", {
        "int* f() { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors());  // F5 audit fix
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u)
        << "`return 0;` from an int*-returning function is a null "
           "pointer conversion per C §6.3.2.3.3";
}

TEST(SemanticAnalyzerC, NullPointerConstantAdmitsAsInit) {
    auto cu = buildShippedUnit("c", {
        "void f() { int* p = 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors());  // F5 audit fix
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "`int* p = 0;` initializer is a null pointer constant";
}

// ── D-CSUBSET-POINTER-COMPAT-ADDRESSOF-INITIALIZER — an address-of expression
//    used as a pointer variable's INITIALIZER (`T *p = &obj;`) must run the SAME
//    C 6.5.16.1 pointer-compatibility check (isAssignable) that the assignment
//    form (`p = &obj;`) and the pointer-variable-init form (`T *p = q;`) already
//    run. Pre-fix, ONLY the address-of INITIALIZER escaped the check.
//
//    Root cause: c's varDecl is declarator-mode, so a scalar initializer's
//    type is derived by a stamped-`typeAt` walk down the single-child wrapper
//    chain (pass2Post). An address-of expression node carries NO stamped type on
//    that chain, so the walk yielded InvalidType, the isAssignable gate
//    (`initTy.valid()`) was skipped, and an INCOMPATIBLE pointee (`char* <- &long`)
//    was SILENTLY accepted. The fix re-derives a scalar (non-brace) initializer's
//    type via subtreeType WITH the declaration's scope when the stamped walk finds
//    nothing, so `&obj` types as Ptr<pointee> and reaches the existing check. The
//    brace-init-list form is explicitly excluded (its per-element checks live in
//    the HIR lowering; a DFS would surface an element's type and false-fire). ──

// (a) INCOMPATIBLE object pointee, char* target: `char *p = &a` where `a` is
// `long`. The pointee `long` is NOT compatible with `char`, so C 6.5.16.1
// requires a diagnostic. Exactly ONE S_TypeMismatch.
// RED-ON-DISABLE: revert the subtreeType-with-scope fallback (stamped-walk only)
// -> `&a` types as InvalidType, the isAssignable gate is skipped, and this count
// drops to 0 (the silent-accept bug this anchor closes).
TEST(SemanticAnalyzerC, PtrInitFromAddressOfIncompatibleCharFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "int main(void) { long a; char *p = &a; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "char* <- &long (incompatible object pointee) INITIALIZER must fail "
           "loud — the address-of initializer runs the same C 6.5.16.1 check the "
           "assignment form `p = &a` already runs";
}

// (b) INCOMPATIBLE object pointee, int* target: `int *p = &a` where `a` is `long`
// (distinct integer types, distinct pointee). Exactly ONE S_TypeMismatch.
// RED-ON-DISABLE: same as (a) — pre-fix this silently accepted.
TEST(SemanticAnalyzerC, PtrInitFromAddressOfIncompatibleIntFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "int main(void) { long a; int *p = &a; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "int* <- &long (incompatible object pointee) INITIALIZER must fail loud";
}

// (c) LEGAL address-of initializers must STAY CLEAN. Each is a conversion the
// existing isAssignable already admits — the fix only makes the check REACH the
// address-of initializer, it does NOT change the rule:
//   void* <- &long   (C 6.3.2.3 — void* takes any object pointer)
//   char* <- &char   (identical pointee)
//   long* <- &long   (identical pointee)
//   int*  <- &int    (identical pointee)
// GREEN-BOTH-WAYS: these are clean pre- AND post-fix; the guard is that the
// tightening did not over-reach into a legal object-pointer init.
TEST(SemanticAnalyzerC, PtrInitFromAddressOfLegalFormsStayClean) {
    auto cu = buildShippedUnit("c", {
        "int main(void) {\n"
        "  long a; char c; int i;\n"
        "  void *vp = &a;\n"   // void* <- any object pointer — legal (C 6.3.2.3)
        "  char *cp = &c;\n"   // char* <- &char (same pointee) — legal
        "  long *lp = &a;\n"   // long* <- &long (same pointee) — legal
        "  int  *ip = &i;\n"   // int*  <- &int  (same pointee) — legal
        "  return (vp != 0) + (cp != 0) + (lp != 0) + (ip != 0);\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "void*<-&obj, and same-pointee &-inits (char/long/int) must ALL stay "
           "accepted — the fix must not over-tighten a legal object-pointer init";
}

// (d) A string-literal initializer of a char* (`char *p = "hi";`) is a legal
// array-to-pointer init and must STAY CLEAN — the fix must not disturb it (the
// stamped walk already finds the literal's array type; subtreeType agrees).
TEST(SemanticAnalyzerC, PtrInitFromStringLiteralStaysClean) {
    auto cu = buildShippedUnit("c", {
        "int main(void) { char *p = \"hi\"; return p != 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "`char *p = \"hi\";` is a legal string-literal pointer init — the "
           "address-of-initializer tightening must not touch it";
}

// (e) PARITY: the address-of INITIALIZER form and the pointer-VARIABLE
// initializer form must reject an incompatible pointee IDENTICALLY. In one TU,
// `char *p = &a;` (address-of init) AND `char *q = lp;` (pointer-var init, where
// `lp` is `long*`) each fire — exactly TWO S_TypeMismatch, proving the address-of
// initializer is no longer the lone unchecked init position.
// RED-ON-DISABLE: pre-fix only the pointer-VARIABLE init fires -> count is 1.
TEST(SemanticAnalyzerC, PtrInitAddressOfAndPointerVarRejectIdentically) {
    auto cu = buildShippedUnit("c", {
        "int main(void) {\n"
        "  long a; long *lp = &a;\n"   // long* <- &long — legal (no mismatch)
        "  char *p = &a;\n"            // char* <- &long — mismatch #1 (address-of init)
        "  char *q = lp;\n"            // char* <- long* — mismatch #2 (pointer-var init)
        "  return (p != 0) + (q != 0);\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 2u)
        << "the address-of initializer AND the pointer-variable initializer must "
           "each reject the char* <- long-pointee pair — two S_TypeMismatch, not one";
}

// 2nd-order audit pin (code-reviewer Critical, step 13.3a): the
// initial F1 fix used arity-based `OperatorTable.lookup(tk, Prefix)`,
// which would have incorrectly matched binary-arithmetic operator
// tokens (MinusOp/PlusOp/StarOp/BitAndOp are registered for BOTH
// Prefix and Infix arities at the SAME SchemaTokenId in
// c.lang.json). The position-based fix only fires when
// the FIRST visible child is a prefix-capable token — distinguishing
// `-x` (first-position) from `a-b` (first-position is `a`). This
// pin asserts S_TypeMismatch STILL fires on `f(1+1)` where `f`
// takes a pointer; pre-position-fix it would have silently
// cascade-suppressed.
TEST(SemanticAnalyzerC, InfixArithmeticStillFiresMismatchAtCallArg) {
    auto cu = buildShippedUnit("c", {
        "extern void f(char* p);\n"
        "int main() { f(1+1); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, NegativeZeroAdmitsAsNullPointerConstant) {
    auto cu = buildShippedUnit("c", {
        "extern void f(void* p);\n"
        "int main() { f(-0); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors())
        << "`-0` folds to integer 0 → a null pointer constant (C §6.3.2.3p3)";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u);
}

// R2: the folded-zero null-pointer admit fires at ALL THREE conversion contexts —
// call-arg, return, and initializer (the shared `admitsNullPointerConstant` site,
// reached from checkCallAgainstSig, checkReturn, and pass-2 decl-init). `1 - 1` /
// `2 - 2` are non-literal integer constant expressions folding to 0.
TEST(SemanticAnalyzerC, FoldedZeroAdmitsAsPointerArg) {
    auto cu = buildShippedUnit("c", {
        "extern void f(void* p);\n"
        "int main() { f(1 - 1); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "`1 - 1` folds to 0 → null pointer constant at a call arg";
}

TEST(SemanticAnalyzerC, FoldedZeroAdmitsAsReturn) {
    auto cu = buildShippedUnit("c", {
        "int* g() { return 1 - 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 0u)
        << "`return 1 - 1;` from an int*-returning function is a null constant";
}

TEST(SemanticAnalyzerC, FoldedZeroAdmitsAsInit) {
    auto cu = buildShippedUnit("c", {
        "void f() { int* p = 2 - 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
// fold to 0 (e.g. a bare `_Bool`/`char`-typed constant — NOT a comparison, which
// now types as C's int per D-CSUBSET-SIZEOF-COMPARISON-INT-TYPE).
TEST(SemanticAnalyzerC, FloatZeroRejectsAsPointerArg) {
    auto cu = buildShippedUnit("c", {
        "extern void f(void* p);\n"
        "int main() { f(1.5 - 1.5); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, FoldedNullMarkerIsTreeKeyedAcrossSources) {
    auto cu = buildShippedUnit("c", {
        "int* a() { return 1 - 1; }\n",
        "int* b() { return 2 - 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, MixedWidthBinaryArgTypedByUacNotLeaf) {
    auto cu = buildShippedUnit("c", {
        "extern int sink(float v);\n"
        "int f(int* a, int b) { return sink(a + b); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ParenWrappedDistinctTypedPointersStillMismatch) {
    auto cu = buildShippedUnit("c", {
        "extern int handler(int* p);\n"
        "int main() {\n"
        "    char* s;\n"
        "    return handler((s));\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, StructMemberAccessViaArrowOnBareRefIsClean) {
    auto cu = buildShippedUnit("c", {
        "struct S { int x; };\n"
        "void f(struct S *p) { p->x = 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NotAPointer), 0u)
        << "p->x where p is Ptr<Struct> must NOT fire S_NotAPointer";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NotAComposite), 0u)
        << "p->x where Struct has field x must NOT fire S_NotAComposite";
}

TEST(SemanticAnalyzerC, ArrowAccessOnNonPointerFiresLoud) {
    auto cu = buildShippedUnit("c", {
        "struct S { int x; };\n"
        "void f(int n) { n->x = 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NotAPointer), 1u)
        << "n->x where n is int must fire EXACTLY ONE S_NotAPointer "
           "— pre-subtreeType swap the bare-ref wrapper's InvalidType "
           "silently suppressed this diagnostic class entirely";
}

TEST(SemanticAnalyzerC, StructDotMemberAccessOnBareRefIsClean) {
    auto cu = buildShippedUnit("c", {
        "struct S { int x; };\n"
        "void f() { struct S s; s.x = 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, MemberAccessSizeofResolvesArrayDimension) {
    auto cu = buildShippedUnit("c", {
        "struct S { int x; int y; };\n"
        "int main() { struct S s; int a[sizeof(s.y)]; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    // aggregateLayout MUST be present for an array-dim sizeof to fold at all
    // (nullopt ⇒ deliberate fail-loud). The scalar `int` size (4) is dataModel-
    // driven, independent of these alignment params.
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
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

TEST(SemanticAnalyzerC, StaticAssertSizeofConditionFoldsTrue) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(sizeof(int) == 4, \"int is 4\");\n"
        "int main(void){ return 42; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "sizeof(int)==4 must FOLD true in the static_assert condition";
}

TEST(SemanticAnalyzerC, StaticAssertSizeofConditionFoldsFalseFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(sizeof(int) == 99, \"int is not 99\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
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
TEST(SemanticAnalyzerC, StaticAssertSizeofVlaIsNotConstantFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "int main(void){\n"
        "  volatile int s = 6;\n"
        "  int n = s;\n"
        "  int a[n];\n"                 // a VLA — sizeof a is runtime, not constant
        "  _Static_assert(sizeof a == 24, \"vla sizeof is not a constant\");\n"
        "  return 0;\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 1u)
        << "sizeof of a VLA is not a constant expression — the _Static_assert must "
           "fail loud (const-eval declines), never fold to a compile-time value";
}

// sizeof-of-a-STRUCT in the condition folds (exercises the aggregateLayout path,
// not just the scalar width). `struct S{int a; int b;}` = 8 bytes under natural
// alignment → the assertion passes; the wrong size fails loud.
TEST(SemanticAnalyzerC, StaticAssertSizeofStructConditionFolds) {
    auto cu = buildShippedUnit("c", {
        "struct S { int a; int b; };\n"
        "_Static_assert(sizeof(struct S) == 8, \"S is 8\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "sizeof(struct S)==8 must fold through the aggregateLayout engine";
}

// The C23 1-ARG form with a sizeof condition (message-less) still folds — pins
// that the peel/parse of the 1-arg form does not disturb the sizeof fold.
TEST(SemanticAnalyzerC, StaticAssertSizeof1ArgFolds) {
    auto cu = buildShippedUnit("c", {
        "static_assert(sizeof(int) == 4);\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// ── TF-C101 — two INDEPENDENT C11 gaps that compose in Tcl 9.0's tclDecls.h ──
//
// The witness is `tclDecls.h`'s TCLBOOLWARNING, which writes
//   `(void)(sizeof(struct {_Static_assert(sizeof(*(boolPtr)) <= sizeof(int), "…");
//                          int dummy;}))`
// and needs BOTH of these, each legal C11 on its own and each previously a hard
// parse error (MEASURED at HEAD before this change: `P0001 expected 'ParenClose'
// — got '{'` for (i); `P0009 … got '_Static_assert'` for (ii)):
//
//   (i)  6.7.7p1 + 6.5.3.4p2 — a struct-or-union-specifier WITH a member list is a
//        legal specifier-qualifier-list, so an anonymous struct DEFINITION (not
//        merely a tag reference) is a legal `sizeof` type-name operand. Fixed at
//        the ONE type-name chokepoint, `castTypeBase`, by routing its composite
//        arms through the c25 UNIFIED `structSpec`/`unionSpec`/`enumSpec` instead
//        of the ref-only `structTypeRef`/`unionTypeRef`/`enumTypeRef`.
//   (ii) 6.7.2.1p1 — `struct-declaration` has TWO productions, and the second is
//        `static_assert-declaration`. Fixed in `structBody`/`unionBody` by making
//        the member repeat an inline 2-way alt.
//
// The pins below assert them SEPARATELY before the composed form, because the two
// halves were separately broken and a single composed test would not say which.
//
// ★ EVERY POSITIVE PIN HERE IS PAIRED WITH A FALSE TWIN, and that pairing is the
// point rather than symmetry for its own sake: a `_Static_assert` that PARSES but
// is never EVALUATED would make every positive pin pass while the construct was
// silently inert — and the assertion in the real macro is load-bearing (it is what
// enforces `sizeof(*boolPtr) <= sizeof(int)`). The false twins are what prove the
// condition actually reaches `constIntExpr`. They also pin the FOLDED VALUE: a
// `sizeof` that folded to the wrong number would flip both counts.

// (i) alone — an anonymous struct DEFINITION as the sizeof type-name operand. The
// static_assert here is only the oracle: it reports what the sizeof folded to.
TEST(SemanticAnalyzerC, SizeofAnonymousStructDefinitionFolds) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(sizeof(struct { int dummy; }) == 4, \"anon struct is 4\");\n"
        "_Static_assert(sizeof(struct { int a; int b; }) == 8, \"anon struct is 8\");\n"
        "_Static_assert(sizeof(union { int a; double b; }) == 8, \"anon union is 8\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "sizeof over an INLINE anonymous struct/union definition must parse "
           "(C 6.7.7p1) and lay the body out through the aggregateLayout engine";
}

// The false twin of (i): proves the size is really COMPUTED from the inline body,
// not rubber-stamped. Both assertions must fail — 2, not 1, not 0.
TEST(SemanticAnalyzerC, SizeofAnonymousStructDefinitionWrongSizeFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(sizeof(struct { int dummy; }) == 99, \"not 99\");\n"
        "_Static_assert(sizeof(struct { int a; int b; }) == 4, \"not 4\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 2u)
        << "a WRONG size over an inline anonymous struct must fail loud — the "
           "positive pin above must not be passing on a rubber-stamped fold";
}

// REGRESSION PIN for the castTypeBase swap. Replacing the ref-only arms with the
// unified specifiers must NOT disturb the pre-existing tag-REFERENCE reading in a
// type-name position — `sizeof(struct S)` / `sizeof(union U)` / `sizeof(enum E)`
// all still resolve their tag (body-absent ⇒ the `isTagReference` arm). This is
// the test that would have caught the swap breaking what already worked.
TEST(SemanticAnalyzerC, SizeofNamedCompositeTagStillResolvesAfterUnify) {
    auto cu = buildShippedUnit("c", {
        "struct S { int a; int b; };\n"
        "union  U { int a; double b; };\n"
        "enum   E { E0, E1 };\n"
        "_Static_assert(sizeof(struct S) == 8, \"S is 8\");\n"
        "_Static_assert(sizeof(union U) == 8, \"U is 8\");\n"
        "_Static_assert(sizeof(enum E) == 4, \"E is 4\");\n"
        "int main(void){ return (int)sizeof(struct S); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "a body-ABSENT composite in a type-name is still a tag reference";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownType), 0u)
        << "the unified specifier must resolve the tag exactly as the ref-only "
           "rule it replaced did";
}

// (ii) alone — `_Static_assert` as a struct-declaration (C 6.7.2.1p1), TRUE arm.
TEST(SemanticAnalyzerC, StaticAssertAsStructMemberParses) {
    auto cu = buildShippedUnit("c", {
        "struct S {\n"
        "  _Static_assert(sizeof(int) <= sizeof(long), \"int fits in long\");\n"
        "  int dummy;\n"
        "};\n"
        "union V {\n"
        "  static_assert(sizeof(int) == 4);\n"
        "  int dummy;\n"
        "};\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "a static_assert is a legal struct-declaration in BOTH composite bodies "
           "and in BOTH spellings (reserved + C23 1-arg)";
}

// ★ THE LOAD-BEARING NEGATIVE. Admitting the construct without EVALUATING it would
// be a silent hole: the grammar change alone makes the true pin above pass. These
// three assertions are false and MUST each fail loud, from struct-member position,
// union-member position, and through the aggregateLayout fold.
TEST(SemanticAnalyzerC, StaticAssertFalseInStructMemberFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "struct S {\n"
        "  _Static_assert(1 == 2, \"struct member assert\");\n"
        "  int dummy;\n"
        "};\n"
        "union V {\n"
        "  _Static_assert(sizeof(int) == 99, \"union member assert\");\n"
        "  int dummy;\n"
        "};\n"
        "struct T {\n"
        "  _Static_assert(sizeof(struct S) == 77, \"layout-folded member assert\");\n"
        "  int dummy;\n"
        "};\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 3u)
        << "a FALSE static assertion in member position must fail loud — parsing "
           "it and skipping the check would be a silent hole";
}

// A static_assert is a DECLARATION, not a member: it must mint no field and must
// not perturb the layout. The oracle is the enclosing struct's own size — a
// mistakenly-minted field would change it and this would fail loud.
TEST(SemanticAnalyzerC, StaticAssertStructMemberMintsNoField) {
    auto cu = buildShippedUnit("c", {
        "struct S {\n"
        "  _Static_assert(1, \"leading\");\n"
        "  int a;\n"
        "  _Static_assert(1, \"middle\");\n"
        "  int b;\n"
        "  _Static_assert(1, \"trailing\");\n"
        "};\n"
        "_Static_assert(sizeof(struct S) == 8, \"asserts mint no field\");\n"
        "int main(void){ struct S s; s.a = 1; s.b = 2; return s.a + s.b; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "static_assert declarations in leading/middle/trailing member position "
           "must not add storage — sizeof(struct S) stays 8";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NotAComposite), 0u)
        << "the real fields around the assertions must still resolve";
}

// (i)+(ii) COMPOSED — the literal tclDecls.h TCLBOOLWARNING shape, in both
// polarities. `int *boolPtr` ⇒ 4 <= 4 holds; the false twin uses `long long *`
// ⇒ 8 <= 4, which is exactly the misuse the real macro exists to catch.
TEST(SemanticAnalyzerC, TclBoolWarningComposedFormHolds) {
    auto cu = buildShippedUnit("c", {
        "int main(void){\n"
        "  int b = 0;\n"
        "  int *boolPtr = &b;\n"
        "  (void)(sizeof(struct {_Static_assert(sizeof(*(boolPtr)) <= sizeof(int),"
        " \"sizeof(boolPtr) too large\");int dummy;}));\n"
        "  return 0;\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "the composed Tcl 9 macro must compile clean for a correctly-sized "
           "pointee";
}

TEST(SemanticAnalyzerC, TclBoolWarningComposedFormCatchesOversizedPointee) {
    auto cu = buildShippedUnit("c", {
        "int main(void){\n"
        "  long long b = 0;\n"
        "  long long *boolPtr = &b;\n"
        "  (void)(sizeof(struct {_Static_assert(sizeof(*(boolPtr)) <= sizeof(int),"
        " \"sizeof(boolPtr) too large\");int dummy;}));\n"
        "  return 0;\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 1u)
        << "an 8-byte pointee must trip the macro's assertion — this is the check "
           "the construct exists to perform, and admitting it inertly would lose it";
}

// ── C11/C23 6.5.3.4 _Alignof — the alignof-folding requirement ───────────────
//
// `_Static_assert(_Alignof(T)==N, ...)` const-evaluates the alignof through the
// SAME `constIntExpr` evaluator that folds sizeof — proving _Alignof is
// const-evaluable AND yields the EXACT alignment. Mirrors the sizeof pins above:
// a true alignof passes, a false one fails loud (not a rubber-stamp). Both
// spellings (`_Alignof`/`alignof`) and a struct type are exercised. The align
// resolver reads the SAME aggregateLayout params analyze() is given.
TEST(SemanticAnalyzerC, StaticAssertAlignofIntFoldsTrue) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(_Alignof(int) == 4, \"int aligns 4\");\n"
        "int main(void){ return 42; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "_Alignof(int)==4 must FOLD true (proves alignof is const-evaluable)";
}

TEST(SemanticAnalyzerC, StaticAssertAlignofDoubleFoldsTrue) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(_Alignof(double) == 8, \"double aligns 8\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "_Alignof(double)==8 must fold true";
}

// The C23 `alignof` spelling folds to alignment 1 for char.
TEST(SemanticAnalyzerC, StaticAssertAlignofCharSpellingFoldsTrue) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(alignof(char) == 1, \"char aligns 1\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "alignof(char)==1 must fold true (the C23 spelling)";
}

// D-CSUBSET-ALIGNOF-GNU-SPELLING: `__alignof__` and `__alignof` are keyword-table
// ALIASES onto the SAME AlignofKeyword `_Alignof` uses, so they must reach this
// identical const-eval path — not merely parse. Both GNU spellings in one unit;
// `_Alignof` is present too as the matched control, so a run in which the whole
// static-assert machinery silently stopped evaluating cannot look like a pass.
TEST(SemanticAnalyzerC, GnuAlignofSpellingsFoldLikeIsoSpelling) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(__alignof__(double) == 8, \"gnu double aligns 8\");\n"
        "_Static_assert(__alignof(int) == 4, \"gnu int aligns 4\");\n"
        "_Static_assert(__alignof__(char) == _Alignof(char), \"alias == iso\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "both GNU alignof spellings must fold through the SAME alignofType "
           "rule as _Alignof";
}

// FAIL-CLOSED twin of the above, and it is not decoration: a `_Static_assert`
// whose operand never got evaluated raises NOTHING, which is indistinguishable
// from a passing assertion. This arm proves the alias's VALUE is really read —
// `__alignof__(double)` is 8, so `== 4` must fail LOUD.
TEST(SemanticAnalyzerC, GnuAlignofSpellingFoldsFalseFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(__alignof__(double) == 4, \"wrong on purpose\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 1u)
        << "__alignof__(double)==4 must FOLD FALSE and fail loud — otherwise the "
           "true-arm above would pass on an alias that parses and evaluates to "
           "nothing";
}

// _Alignof of a STRUCT = the MAX member alignment (not the size): {char; double}
// is 16 bytes but aligns to 8 (the double). Exercises the aggregateLayout path
// and proves alignof reads ALIGNMENT, never size.
TEST(SemanticAnalyzerC, StaticAssertAlignofStructFoldsToMaxMemberAlign) {
    auto cu = buildShippedUnit("c", {
        "struct CharDouble { char c; double d; };\n"
        "_Static_assert(_Alignof(struct CharDouble) == 8, \"aligns 8\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "_Alignof(struct{char;double;})==8 (max member align, NOT the size 16)";
}

TEST(SemanticAnalyzerC, StaticAssertAlignofFoldsFalseFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(_Alignof(double) == 4, \"wrong on purpose\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
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
TEST(SemanticAnalyzerC, AlignasVariableValueFormParses) {
    auto cu = buildShippedUnit("c", { "alignas(16) int x;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    SymbolRecord const* x = findSym(model, "x");
    ASSERT_NE(x, nullptr);
    EXPECT_TRUE(x->type.valid());
}

// PARSE: the TYPE operand form `alignas(double) int y;` parses (a type-name in
// the alignas operand contributes _Alignof(double)==8, which is ≥ int's 4, so
// no weaker-than-natural error).
TEST(SemanticAnalyzerC, AlignasVariableTypeFormParses) {
    auto cu = buildShippedUnit("c", { "alignas(double) int y;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasWeakerThanNatural), 0u);
    SymbolRecord const* y = findSym(model, "y");
    ASSERT_NE(y, nullptr);
    // alignas(double) = 8 on an int (natural 4) — a valid RAISE.
    ASSERT_TRUE(y->explicitAlignment.has_value());
    EXPECT_EQ(*y->explicitAlignment, 8u);
}

// PARSE: a struct member `struct S { alignas(16) int a; char b; };` parses.
TEST(SemanticAnalyzerC, AlignasStructMemberParses) {
    auto cu = buildShippedUnit("c",
                               { "struct S { alignas(16) int a; char b; };\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    // No alignas constraint diagnostics at all for a valid raise.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasInvalidContext), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasWeakerThanNatural), 0u);
}

// VARIABLE STORAGE: `alignas(32) int g;` sets SymbolRecord.explicitAlignment==32.
// (The stored value is intentionally NOT consumed by variable codegen yet — that
// is a separate deferred task; here we assert only that the SEMANTIC store works.)
TEST(SemanticAnalyzerC, AlignasVariableStoresExplicitAlignment) {
    auto cu = buildShippedUnit("c", { "alignas(32) int g;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    SymbolRecord const* g = findSym(model, "g");
    ASSERT_NE(g, nullptr);
    ASSERT_TRUE(g->explicitAlignment.has_value())
        << "alignas(32) must set SymbolRecord.explicitAlignment";
    EXPECT_EQ(*g->explicitAlignment, 32u);
}

// VALUE-EXPR STORAGE: `alignas(2*8) int g;` const-folds the operand to 16.
TEST(SemanticAnalyzerC, AlignasVariableConstExprOperandFolds) {
    auto cu = buildShippedUnit("c", { "alignas(2*8) int g;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    SymbolRecord const* g = findSym(model, "g");
    ASSERT_NE(g, nullptr);
    ASSERT_TRUE(g->explicitAlignment.has_value());
    EXPECT_EQ(*g->explicitAlignment, 16u);
}

// MEMBER LAYOUT END-TO-END (via the interner): `struct S { alignas(16) char c; }`
// → _Alignof(struct S)==16 AND sizeof(struct S)==16 (the alignas raised BOTH the
// struct's alignment and its rounded size). Reuses the _Static_assert(_Alignof())
// fold — RED-ON-DISABLE: without the fieldAligns wiring the struct aligns to 1.
TEST(SemanticAnalyzerC, AlignasMemberRaisesStructAlignAndSizeEndToEnd) {
    auto cu = buildShippedUnit("c", {
        "struct S { alignas(16) char c; };\n"
        "_Static_assert(_Alignof(struct S) == 16, \"aligns 16\");\n"
        "_Static_assert(sizeof(struct S) == 16, \"sizes 16\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "alignas(16) on the sole char member must raise the struct to "
           "_Alignof==16 AND sizeof==16 (end-to-end via fieldAligns)";
}

// MEMBER LAYOUT — following-field OFFSET: `struct T { char c; alignas(8) int i; }`
// pushes `i` from its natural offset 4 to 8. Proven via the _Alignof of the
// struct (max member align == 8) plus its size: char(1)+pad(7)+int(4) rounded to
// 8 → 16. (The offsetof idiom itself is exercised in the corpus/e2e probe.)
TEST(SemanticAnalyzerC, AlignasMemberRaisesFollowingFieldLayout) {
    auto cu = buildShippedUnit("c", {
        "struct T { char c; alignas(8) int i; };\n"
        "_Static_assert(_Alignof(struct T) == 8, \"aligns 8\");\n"
        "_Static_assert(sizeof(struct T) == 16, \"sizes 16\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// MEMBER LAYOUT — UNION: `union U { alignas(16) char c; int i; }` raises the
// union to _Alignof==16 and sizeof==16 (the completed carrier + the union-arm
// alignas fold in computeLayout).
TEST(SemanticAnalyzerC, AlignasUnionMemberRaisesAlignAndSizeEndToEnd) {
    auto cu = buildShippedUnit("c", {
        "union U { alignas(16) char c; int i; };\n"
        "_Static_assert(_Alignof(union U) == 16, \"aligns 16\");\n"
        "_Static_assert(sizeof(union U) == 16, \"sizes 16\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
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
TEST(SemanticAnalyzerC, PackedStructGnuRemovesPaddingEndToEnd) {
    auto cu = buildShippedUnit("c", {
        "struct S { char c; int v; } __attribute__((packed));\n"
        "_Static_assert(sizeof(struct S) == 5, \"packed size 5\");\n"
        "_Static_assert(_Alignof(struct S) == 1, \"packed align 1\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "packed must remove padding: sizeof==5 AND _Alignof==1 end-to-end";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u);
}

// C23 spelling: `[[gnu::packed]]` as a trailing attribute is honored identically.
TEST(SemanticAnalyzerC, PackedStructC23GnuPackedSpelling) {
    auto cu = buildShippedUnit("c", {
        "struct S { char c; int v; } [[gnu::packed]];\n"
        "_Static_assert(sizeof(struct S) == 5, \"packed size 5\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// C23 bare `[[packed]]` spelling (no namespace) is honored too.
TEST(SemanticAnalyzerC, PackedStructC23BarePackedSpelling) {
    auto cu = buildShippedUnit("c", {
        "struct S { char c; int v; } [[packed]];\n"
        "_Static_assert(sizeof(struct S) == 5, \"packed size 5\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// packed + a member `alignas` — alignas WINS per-field even under packed:
// `struct S {char c; alignas(4) int v;} __attribute__((packed));` → v@4, sizeof 8.
TEST(SemanticAnalyzerC, PackedStructMemberAlignasStillRaises) {
    auto cu = buildShippedUnit("c", {
        "struct S { char c; alignas(4) int v; } __attribute__((packed));\n"
        "_Static_assert(_Alignof(struct S) == 4, \"alignas wins\");\n"
        "_Static_assert(sizeof(struct S) == 8, \"v raised to offset 4\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "a member alignas raises per-field even inside a packed struct";
}

// `alignas(1)` INSIDE a packed struct is LEGAL (the member's natural baseline is 1
// under packed), so NO S_AlignasWeakerThanNatural. Contrast:
// `AlignasWeakerThanNaturalFailsLoud` — `alignas(1) double d;` OUTSIDE a packed
// struct still fails. RED-ON-DISABLE: drop the packed naturalBaseline and this fires.
TEST(SemanticAnalyzerC, AlignasOneInsidePackedStructIsLegal) {
    auto cu = buildShippedUnit("c", {
        "struct S { char c; alignas(1) int v; } __attribute__((packed));\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasWeakerThanNatural), 0u)
        << "alignas(1) inside a packed struct is legal (baseline 1)";
    EXPECT_FALSE(model.hasErrors());
}

// packed UNION: `union U {char c; int i;} __attribute__((packed));` → _Alignof 1.
TEST(SemanticAnalyzerC, PackedUnionHasAlignmentOneEndToEnd) {
    auto cu = buildShippedUnit("c", {
        "union U { char c; int i; } __attribute__((packed));\n"
        "_Static_assert(_Alignof(union U) == 1, \"packed union align 1\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// A trailing packed attribute does NOT block a following declarator:
// `struct S {...} __attribute__((packed)) g;` — `g` still parses, S stays packed.
TEST(SemanticAnalyzerC, PackedStructFollowedByDeclaratorParses) {
    auto cu = buildShippedUnit("c", {
        "struct S { char c; int v; } __attribute__((packed)) g;\n"
        "_Static_assert(sizeof(struct S) == 5, \"packed size 5\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
    EXPECT_NE(findSym(model, "g"), nullptr);
}

// FAIL-LOUD: packed + a bit-field member → S_PackedBitfieldUnsupported (bit-granular
// packed packing is a distinct, deferred algorithm —
// D-CSUBSET-PACKED-BITFIELD-INTERACTION). NEVER a silent NON-packed layout.
TEST(SemanticAnalyzerC, PackedBitfieldFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "struct S { int a : 3; } __attribute__((packed));\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_PackedBitfieldUnsupported), 1u);
    EXPECT_TRUE(model.hasErrors());
}

// ★★ TF-C82 (D-PP-PRAGMA-REGISTRY): the `#pragma pack` AMBIGUITY pair. These two
// tests exist together because the FIRST implementation had only the second half
// and MEASURED it refused a program clang compiles.
//
// (1) LEGITIMATE, must compile: a shared MEMBER macro expanded under TWO
// different caps. Its replacement tokens really are stamped twice, but every
// composite using it is anchored on its own unambiguous `struct` keyword, so
// both layouts are fully determined. Raising the conflict where it is DETECTED
// (in the preprocessor) rejects this; raising it where it is USED does not.
TEST(SemanticAnalyzerC, PragmaPackSharedMemberMacroAcrossCapsIsFine) {
    auto cu = buildShippedUnit("c", {
        "#define MEMS unsigned a; long long b;\n"
        "#pragma pack(4)\n"
        "struct A { MEMS };\n"
        "#pragma pack(2)\n"
        "struct B { MEMS };\n"
        "#pragma pack()\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_PragmaPackAmbiguous), 0u)
        << "the composites are anchored on unambiguous `struct` keywords — "
           "clang compiles this, and MEASURED, so must DSS";
    EXPECT_FALSE(model.hasErrors());
}

// (2) GENUINELY AMBIGUOUS, must fail loud: the COMPOSITE ITSELF is minted by a
// macro expanded under two different caps, so its layout key carries both. The
// two candidate layouts differ in size AND in every field offset past the first;
// picking one silently is the miscompile this whole cycle is about. ONE
// diagnostic per affected composite, and the code is unsuppressable.
TEST(SemanticAnalyzerC, PragmaPackAmbiguousCompositeFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "#define DEFS(n) struct n { unsigned a; long long b; };\n"
        "#pragma pack(4)\n"
        "DEFS(P)\n"
        "#pragma pack(2)\n"
        "DEFS(Q)\n"
        "#pragma pack()\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_PragmaPackAmbiguous), 2u)
        << "both P and Q land on an ambiguous layout key";
    EXPECT_TRUE(model.hasErrors());
}

// FAIL-LOUD: a TYPO in the GNU `__attribute__` packed slot → S_UnknownTypeAttribute
// (typo protection, like H_UnknownLinkageSpecifier — a `pakced` typo must not
// silently leave the struct unpacked).
TEST(SemanticAnalyzerC, UnknownGnuTypeAttributeFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "struct S { int x; } __attribute__((pakced));\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 1u);
    EXPECT_TRUE(model.hasErrors());
}

// STANDARD-IGNORABLE: an unrecognized C23 `[[...]]` attribute on a struct is
// ignored (C23 6.7.11.1 — an unknown attribute is ignored), NO diagnostic. This is
// the `[[deprecated]]` precedent; only `packed`/`gnu::packed` are honored-or-diagnosed.
TEST(SemanticAnalyzerC, UnknownC23AttributeIsIgnored) {
    auto cu = buildShippedUnit("c", {
        "struct S { int x; } [[deprecated]];\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// ── D-CSUBSET-PACKED-AFTER-KEYWORD-POSITION — DEFERRAL CLOSED (TF-C73) ───────
//
// ★★ THESE TWO PINS WERE INVERTED ON 2026-07-28. They shipped as
// `PackedAfterKeywordTaggedFailsLoudNotSilent` / `…AnonymousFailsLoudNotSilent`
// and asserted that `struct __attribute__((packed)) S {…}` must be LOUDLY
// REJECTED. That was the CORRECT reading of the fail-loud contract for exactly
// as long as the position was DEFERRED: a deferred construct must never be
// accepted-as-unpacked, so while the grammar admitted `compositeAttrList` only
// as a TRAILING element, rejection WAS the safety property. The position has
// since LANDED (`compositeAttrLead` in `shapes`, wired through the
// structSpec/unionSpec rows' `declarationAttrSlotRules`) and is HONORED, so the
// safety property is now satisfied by HONORING, not by rejecting — the struct
// is correctly PACKED rather than silently unpacked. The old assertion no
// longer guards anything; it merely pins the absence of a feature that exists.
// The pins are kept, and kept HERE, because their real subject was never the
// diagnostic — it was the POSITIONAL SYMMETRY between the after-keyword slot
// and the trailing slot, and that intent survives the inversion intact.
//
// ★★ WHY THESE ASSERT A LAYOUT FACT AND NOT A DIAGNOSTIC COUNT. Post-landing,
// EVERY regression mode of this feature is SILENT — there is no diagnostic to
// count in either direction, so a count pin here would be permanently dead.
// MEASURED, not argued (shipped CLI, arm64 macho, `--target
// arm64:macho64-arm64-darwin-exec`, probe returning `sizeof*10 + _Alignof`):
//
//     shipped config                                   -> 51   (sizeof 5, align 1)
//     scratch config, `declarationAttrSlotRules`
//       ["compositeAttrLead"] deleted from the
//       structSpec + unionSpec rows                     -> 84   (sizeof 8, align 4)
//     diagnostics emitted by that broken build          -> 0
//
// Zero. The regression compiles clean and produces a WRONG ABI. Only an
// applied-layout assertion — `isPacked`, the computed size/align, the field
// OFFSETS — can see it, which is what these two now assert.
//
// ★★ CLANG GROUND TRUTH, measured not assumed
// (`clang -isysroot $(xcrun --show-sdk-path) -std=c17 -Wall -Wextra`, zero
// errors, zero warnings, binary run):
//     struct __attribute__((packed)) S { char a; int b; };
//         -> sizeof 5, _Alignof 1, offsetof(b) == 1
//     struct __attribute__((packed))   { char a; int b; } v;   (anonymous)
//         -> sizeof 5,             offsetof(b) == 1
// DSS agrees on both, end to end through the shipped CLI, and the
// attribute-deleted control diverges in BOTH compilers identically (sizeof 8).
// The member order is `char` FIRST deliberately: with `int a; char b;` the two
// field offsets are 0/4 whether or not packing ran, so the offsets would be
// vacuous. `char a; int b;` moves all three observables at once (5 vs 8, 1 vs
// 4, offset 1 vs 4) — one shape, three independent ways to catch a silent drop.
//
// ★ WHAT GUARDS THE BEHAVIOR NOW, i.e. what these pins actually hold down:
//   * the lead surface REACHES the semantic tier at all — the config key
//     `declarationAttrSlotRules: ["compositeAttrLead"]`, deleted above to
//     produce the 84;
//   * `scanCompositePacked` stays surface-COUNT-agnostic. It once selected the
//     FIRST `compositeAttrList` child and `break`ed, which is precisely why an
//     earlier cut of this grammar was reverted UNSHIPPED: a lead slot under a
//     distinct rule name was invisible to it and `packed` — known-and-inert in
//     the effects table — was dropped in silence;
//   * the tag stays at visible-child 2. `compositeAttrLead` is a named rule
//     over a lone `{repeat}`, so it emits its node even when EMPTY and the
//     index cannot shift with or without a decoration. (That index has its own
//     red-on-disable note on the structSpec row; it fails as a downstream
//     S000D at the USE site, not as an unknown tag.)
// The TRAILING position keeps its own coverage in the pins above
// (`PackedStructHasAlignmentOneEndToEnd` and siblings); the positional-symmetry
// claim is the pair of them, which is why these two stay adjacent to it.
TEST(SemanticAnalyzerC, PackedAfterKeywordTaggedIsHonoredNotSilentlyUnpacked) {
    auto cu = buildShippedUnit("c", {
        "struct __attribute__((packed)) S { char a; int b; };\n"
        "struct S v;\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_FALSE(model.hasErrors())
        << "the after-keyword composite attribute position LANDED — it must parse "
           "and analyze cleanly (D-CSUBSET-PACKED-AFTER-KEYWORD-POSITION, closed)";
    // The tag must survive the lead slot: `struct S v;` resolving to a Struct is
    // the guard on `structSpec`'s name index staying at visible-child 2.
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr) << "the TAG was discarded — the lead slot shifted the "
                             "name index off the identifier";
    ASSERT_TRUE(v->type.valid());
    auto const& ti = model.lattice().interner();
    ASSERT_EQ(ti.kind(v->type), TypeKind::Struct);
    // THE APPLIED LAYOUT FACT. `packed` reached the layout sink, not merely the
    // parser: clang-measured 5 / 1 / offset 1; a silent drop gives 8 / 4 / 4.
    EXPECT_TRUE(ti.isPacked(v->type))
        << "after-keyword `packed` parsed but never marked the composite — the "
           "silent-unpack regression (MEASURED as sizeof 8 vs clang's 5)";
    auto const layout = computeLayout(v->type, ti, kAlignasLayout, DataModel::Lp64);
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->size, 5u)          << "clang: sizeof == 5 (unpacked would be 8)";
    EXPECT_EQ(layout->align.bytes(), 1u) << "clang: _Alignof == 1 (unpacked would be 4)";
    ASSERT_EQ(layout->fieldOffsets.size(), 2u);
    EXPECT_EQ(layout->fieldOffsets[0], 0u);
    EXPECT_EQ(layout->fieldOffsets[1], 1u)
        << "clang: offsetof(b) == 1 — the inter-field padding is gone (unpacked: 4)";
}

// The ANONYMOUS half of the positional-symmetry claim: the same lead slot on a
// TAGLESS composite. Not redundant with the tagged pin — the tagged one also
// exercises the tag-index guard, and this one exercises the path where
// `anonymousNameAllowed` synthesizes the name while child 2 is a `structBody`
// rather than an Identifier. Same clang-measured layout (5 / offset 1); DSS
// end-to-end MEASURED at exit 42 on the runtime probe and at exit 1 with the
// attribute deleted, in DSS and clang alike.
TEST(SemanticAnalyzerC, PackedAfterKeywordAnonymousIsHonoredNotSilentlyUnpacked) {
    auto cu = buildShippedUnit("c", {
        "struct __attribute__((packed)) { char a; int b; } v;\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_FALSE(model.hasErrors())
        << "anonymous after-keyword packed must parse + analyze cleanly";
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->type.valid());
    auto const& ti = model.lattice().interner();
    ASSERT_EQ(ti.kind(v->type), TypeKind::Struct);
    EXPECT_TRUE(ti.isPacked(v->type))
        << "the lead slot must be honored on an ANONYMOUS composite too — never a "
           "silent accept-as-unpacked";
    auto const layout = computeLayout(v->type, ti, kAlignasLayout, DataModel::Lp64);
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->size, 5u)          << "clang: sizeof == 5 (unpacked would be 8)";
    EXPECT_EQ(layout->align.bytes(), 1u);
    ASSERT_EQ(layout->fieldOffsets.size(), 2u);
    EXPECT_EQ(layout->fieldOffsets[1], 1u)
        << "clang: offsetof(b) == 1 (unpacked: 4)";
}

// ZERO: `alignas(0) int x;` is a NO-OP (6.7.5p3) — NO diagnostic, NO override.
TEST(SemanticAnalyzerC, AlignasZeroIsNoOpNoOverride) {
    auto cu = buildShippedUnit("c", { "alignas(0) int x;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
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
TEST(SemanticAnalyzerC, AlignasNotPowerOfTwoFailsLoud) {
    auto cu = buildShippedUnit("c", { "alignas(3) int x;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNotPowerOfTwo), 1u);
}

// CONSTRAINT: a value over the 256-byte cap → S_AlignasExceedsMax (a distinct
// code from not-power-of-two — 512 IS a power of two, just too large).
TEST(SemanticAnalyzerC, AlignasExceedsMaxFailsLoud) {
    auto cu = buildShippedUnit("c", { "alignas(512) int x;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasExceedsMax), 1u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNotPowerOfTwo), 0u);
}

// CONSTRAINT: an alignment WEAKER than the declared type's natural alignment →
// S_AlignasWeakerThanNatural (6.7.5p4: alignas may only strengthen; 1 < 8).
TEST(SemanticAnalyzerC, AlignasWeakerThanNaturalFailsLoud) {
    auto cu = buildShippedUnit("c", { "alignas(1) double d;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasWeakerThanNatural), 1u);
}

// CONSTRAINT: a non-constant value operand → S_AlignasNonConstant.
TEST(SemanticAnalyzerC, AlignasNonConstantFailsLoud) {
    auto cu = buildShippedUnit("c",
                               { "int nc; alignas(nc) int x;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNonConstant), 1u);
}

// CONSTRAINT: a NEGATIVE value is a constraint violation (NOT the 6.7.5p3 zero
// no-op) → S_AlignasNotPowerOfTwo. Fail-loud: `alignas(-4)` must NOT be silently
// swallowed as "no alignment" (a negative is not a valid alignment; gcc/clang
// both reject it). RED-ON-DISABLE: were `value <= 0` treated as a no-op, this
// would compile with ZERO diagnostics — a silent constraint violation.
TEST(SemanticAnalyzerC, AlignasNegativeFailsLoud) {
    auto cu = buildShippedUnit("c", { "alignas(-4) int x;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNotPowerOfTwo), 1u)
        << "alignas(-4) is a constraint violation, not a no-op — fail loud";
}

// EXACTLY-ONE diagnostic across a MULTI-DECLARATOR declaration: the alignas lives
// on the shared prefix, so `alignas(3) int a, b;` is ONE erroneous specifier →
// ONE S_AlignasNotPowerOfTwo (not one per declarator). RED-ON-DISABLE for the
// per-declaration emit gate: without it the diagnostic fires twice.
TEST(SemanticAnalyzerC, AlignasMultiDeclaratorEmitsExactlyOnce) {
    auto cu = buildShippedUnit("c", { "alignas(3) int a, b;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNotPowerOfTwo), 1u)
        << "a shared-prefix alignas error must be reported once, not per declarator";
}

// MULTI-DECLARATOR STORE: a VALID `alignas(16) int a, b;` stores the override on
// EVERY declarator's symbol (the prefix applies to all slots).
TEST(SemanticAnalyzerC, AlignasMultiDeclaratorStoresOnAll) {
    auto cu = buildShippedUnit("c", { "alignas(16) int a, b;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
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
TEST(SemanticAnalyzerC, AlignasOnFunctionFailsLoud) {
    auto cu = buildShippedUnit("c",
                               { "alignas(16) int f(void);\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasInvalidContext), 1u);
}

// CONSTRAINT: alignas on a BIT-FIELD member → S_AlignasInvalidContext (6.7.5p2).
TEST(SemanticAnalyzerC, AlignasOnBitFieldMemberFailsLoud) {
    auto cu = buildShippedUnit("c",
                               { "struct S { alignas(8) int a : 3; };\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasInvalidContext), 1u);
}

// The C11 spelling `_Alignas` works identically to the C23 `alignas`.
TEST(SemanticAnalyzerC, AlignasC11SpellingStores) {
    auto cu = buildShippedUnit("c", { "_Alignas(64) int g;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
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
TEST(SemanticAnalyzerC, AlignasEnumConstantOperandFolds) {
    auto cu = buildShippedUnit("c", {
        "enum E { W = 16 };\n"
        "alignas(W) int g;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
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
TEST(SemanticAnalyzerC, AlignasSizeofOperandFolds) {
    auto cu = buildShippedUnit("c", { "alignas(sizeof(double)) int g;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
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
TEST(SemanticAnalyzerC, TypeofUnqualStripsVolatile) {
    auto cu = buildShippedUnit("c", {
        "typeof_unqual(volatile int) v;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, TypeofPreservesVolatile) {
    auto cu = buildShippedUnit("c", {
        "typeof(volatile int) v;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, TypeofEnumConstantOperandResolvesAsValue) {
    auto cu = buildShippedUnit("c", {
        "enum E { GREEN = 7 };\n"
        "typeof(GREEN) v;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, TypeofExpressionFormResolvesToOperandType) {
    auto cu = buildShippedUnit("c", {
        "int x;\n"
        "typeof(x) y;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const& ti = model.lattice().interner();
    SymbolRecord const* y = findSym(model, "y");
    ASSERT_NE(y, nullptr);
    ASSERT_TRUE(y->type.valid());
    EXPECT_EQ(ti.kind(y->type), TypeKind::I32)
        << "typeof(x) where x is int must resolve y to int";
}

// TYPE-NAME form: `typeof(int*)` resolves to Ptr<int> (castTypeRef operand).
TEST(SemanticAnalyzerC, TypeofTypeNameFormResolvesPointer) {
    auto cu = buildShippedUnit("c", {
        "typeof(int*) p;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const& ti = model.lattice().interner();
    SymbolRecord const* p = findSym(model, "p");
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(p->type.valid());
    ASSERT_EQ(ti.kind(p->type), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(ti.operands(p->type)[0]), TypeKind::I32);
}

// `sizeof(typeof(unsigned short))` folds to 2 in an array dimension — the typeof
// resolves inside the SAME sizeof-fold path sizeof(T)/enum/arithmetic use.
TEST(SemanticAnalyzerC, SizeofTypeofFoldsInArrayDim) {
    auto cu = buildShippedUnit("c", {
        "int a[sizeof(typeof(unsigned short))];\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* a = findSym(model, "a");
    ASSERT_NE(a, nullptr);
    ASSERT_TRUE(a->type.valid());
    ASSERT_EQ(ti.kind(a->type), TypeKind::Array);
    ASSERT_EQ(ti.scalars(a->type).size(), 1u);
    EXPECT_EQ(ti.scalars(a->type)[0], 2)
        << "sizeof(typeof(unsigned short)) folds to 2";
}

// ── D-CSUBSET-TYPEOF-GNU-SPELLING: `__typeof__` / `__typeof` ────────────────
//
// The aliases route to TypeofKeyword, so they must resolve EXACTLY as `typeof`
// does — both operand forms, from the same `typeofSpecifier`.
TEST(SemanticAnalyzerC, GnuTypeofSpellingsResolveLikeIsoSpelling) {
    auto cu = buildShippedUnit("c", {
        "int x;\n"
        "__typeof__(x) a;\n"        // EXPRESSION operand, long spelling
        "__typeof(x) b;\n"          // EXPRESSION operand, short spelling
        "__typeof__(int*) p;\n",    // TYPE-NAME operand
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const& ti = model.lattice().interner();
    for (char const* name : {"a", "b"}) {
        SymbolRecord const* s = findSym(model, name);
        ASSERT_NE(s, nullptr) << name;
        ASSERT_TRUE(s->type.valid()) << name;
        EXPECT_EQ(ti.kind(s->type), TypeKind::I32)
            << name << ": a GNU typeof spelling over an int operand is int";
    }
    SymbolRecord const* p = findSym(model, "p");
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(p->type.valid());
    ASSERT_EQ(ti.kind(p->type), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(ti.operands(p->type)[0]), TypeKind::I32)
        << "__typeof__(int*) resolves through the SAME castTypeRef the ISO "
           "spelling uses";
}

// ★ THE assertion that makes the alias correct rather than merely accepted.
// `typeof` and `typeof_unqual` differ in EXACTLY ONE observable: the top-level
// qualifier strip. GNU `__typeof__` means the PRESERVING one, so a keyword row
// that mapped it to TypeofUnqualKeyword would still parse, still resolve to a
// plain int, and still pass every test above — and would silently drop a
// `volatile` the program wrote. Only this pin separates the two mappings, and
// it is the exact mirror of TypeofUnqualStripsVolatile one section up.
TEST(SemanticAnalyzerC, GnuTypeofSpellingPreservesVolatile) {
    auto cu = buildShippedUnit("c", {
        "__typeof__(volatile int) v;\n"
        "__typeof(volatile int) w;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const& ti = model.lattice().interner();
    for (char const* name : {"v", "w"}) {
        SymbolRecord const* s = findSym(model, name);
        ASSERT_NE(s, nullptr) << name;
        ASSERT_TRUE(s->type.valid()) << name;
        EXPECT_TRUE(ti.isVolatileQualified(s->type))
            << name << ": GNU __typeof__ is the QUALIFIER-PRESERVING typeof, "
                       "never typeof_unqual — a row mapped to "
                       "TypeofUnqualKeyword would strip this and pass "
                       "everything else";
    }
}

// Bit-field operand → S_TypeofBitfieldOperand (C 6.7.2.5 constraint): a bit-field
// has no nameable type. RED-on-disable for the bit-field gate.
TEST(SemanticAnalyzerC, TypeofBitfieldOperandFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "struct S { unsigned f : 3; };\n"
        "struct S s;\n"
        "typeof(s.f) v;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, GenericSelectedBranchTypeIsResultType) {
    auto cu = buildShippedUnit("c", {
        "double f(void){ int i = 0; return _Generic(i, int: 1.5, "
        "long: 2, default: 0); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, GenericNoMatchNoDefaultFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "int main(void){ double d = 0; return _Generic(d, int: 1, char: 2); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_GenericSelectionNoMatch), 1u)
        << "no typed match and no default is a constraint violation";
}

// The `default` fallback is selected when no typed association matches — `char*`
// vs {int, double} → default. No no-match error; the node types the default's
// result type.
TEST(SemanticAnalyzerC, GenericDefaultFallbackSelected) {
    auto cu = buildShippedUnit("c", {
        "int main(void){ char c = 0; char* p = &c; "
        "return _Generic(p, int: 1, double: 2, default: 7); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, GenericValueInTypePositionFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "int main(void){ int i = 0; int notAType = 5; "
        "return _Generic(i, notAType: 1, default: 0); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownType), 1u)
        << "a value identifier in an association type position must fail loud";
}

// Two associations naming the SAME type (compatible types — 6.5.1.1p2 forbids it)
// is ambiguous → S_GenericSelectionAmbiguous.
TEST(SemanticAnalyzerC, GenericAmbiguousMatchFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "int main(void){ int i = 0; "
        "return _Generic(i, int: 1, int: 2, default: 0); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_GenericSelectionAmbiguous), 1u)
        << "two associations of the same type is a constraint violation";
}

// A typedef name in an association type position resolves through the alias and
// matches the underlying type (`MyInt` ≡ `int` → the MyInt-branch wins for an
// `int` controlling expression).
TEST(SemanticAnalyzerC, GenericTypedefAssociationMatches) {
    auto cu = buildShippedUnit("c", {
        "typedef int MyInt;\n"
        "int main(void){ int i = 0; "
        "return _Generic(i, MyInt: 42, double: 3, default: 0); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ForwardRefSizeofArrayDimensionStillRejected) {
    auto cu = buildShippedUnit("c", {
        "int a[sizeof(b)]; int b;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
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
TEST(SemanticAnalyzerC, BadFieldSizeofArrayDimensionRejected) {
    auto cu = buildShippedUnit("c", {
        "struct S { int x; int y; };\n"
        "int main() { struct S s; int a[sizeof(s.nope)]; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
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
TEST(SemanticAnalyzerC, SizeofVaListIs24UnderSysV) {
    auto cu = buildShippedUnit("c", {
        "int a[sizeof(va_list)];\n",
    });
    assertNoBuilderErrors(*cu);
    // SysVRegisterSave (the default/absent strategy): va_list = __va_list_tag[1].
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
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

TEST(SemanticAnalyzerC, SizeofVaListIs8UnderWin64) {
    auto cu = buildShippedUnit("c", {
        "int a[sizeof(va_list)];\n",
    });
    assertNoBuilderErrors(*cu);
    // HomogeneousPointer (Win64): va_list = char* (one pointer = 8B).
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
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
TEST(SemanticAnalyzerC, SizeofVaListIs32UnderAapcs64) {
    auto cu = buildShippedUnit("c", {
        "int a[sizeof(va_list)];\n",
    });
    assertNoBuilderErrors(*cu);
    // Aapcs64DualCursor: va_list = __va_list (the 5-field struct, 32B).
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
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
// the dual-cursor seam is realized (the FC12b-era fail-loud is gone). The c
// `vaArgRule` gates the injection; the body NAMES + USES va_list/va_start/va_arg.
TEST(SemanticAnalyzerC, Aapcs64VariadicCalleeAnalyzesClean) {
    auto cu = buildShippedUnit("c", {
        "int f(int n, ...) { va_list ap; va_start(ap, n);"
        " int t = va_arg(ap, int); va_end(ap); return t; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                         VaListStrategy::Aapcs64DualCursor);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_VariadicCalleeUnsupported), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "the __va_list struct ap operand must pass isVaList for AAPCS64";
}

// D-CSUBSET-BUILTIN-VA-LIST-TYPE-NAME (sqlite os_unix.c/mem1.c/test1.c, always via
// the Darwin SDK's `<sys/_types/_va_list.h>` `typedef __builtin_va_list
// __darwin_va_list;`): `__builtin_va_list` is a COMPILER-PROVIDED type name — no
// shipped descriptor can supply it — injected beside `va_list` in the builtin scope
// at all THREE VaListStrategy branches, bound to the IDENTICAL per-strategy TypeId.
// These pins fold sizeof(__builtin_va_list) into an array dimension exactly like the
// sizeof(va_list) fixtures above; matching the SAME constant per strategy proves the
// alias selects the SAME strategy-typed injection (a missed branch would leave the
// name undefined on exactly that calling convention). RED-ON-DISABLE: reverting the
// three `__builtin_va_list` injections reproduces S0006/S_UnknownType on
// `typedef __builtin_va_list x;` (the sqlite trigger) — every positive test in this
// group references `__builtin_va_list` in analyzed source, so each IS the
// red-on-disable witness (the fold/identity below cannot succeed without them).
TEST(SemanticAnalyzerC, SizeofBuiltinVaListMatchesVaListUnderSysV) {
    auto cu = buildShippedUnit("c", {
        "int a[sizeof(__builtin_va_list)];\n",
    });
    assertNoBuilderErrors(*cu);
    // SysVRegisterSave: __builtin_va_list = va_list = __va_list_tag[1] (24B).
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                         VaListStrategy::SysVRegisterSave);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownType), 0u);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* aRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "a") aRec = &model.symbols()[i];
    ASSERT_NE(aRec, nullptr);
    ASSERT_TRUE(aRec->type.valid());
    ASSERT_EQ(ti.kind(aRec->type), TypeKind::Array);
    ASSERT_EQ(ti.scalars(aRec->type).size(), 1u);
    EXPECT_EQ(ti.scalars(aRec->type)[0], 24)
        << "sizeof(__builtin_va_list) under SysV = sizeof(va_list) = 24";
}

TEST(SemanticAnalyzerC, SizeofBuiltinVaListMatchesVaListUnderWin64) {
    auto cu = buildShippedUnit("c", {
        "int a[sizeof(__builtin_va_list)];\n",
    });
    assertNoBuilderErrors(*cu);
    // HomogeneousPointer (Win64 + Apple arm64): __builtin_va_list = char* (8B).
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                         VaListStrategy::HomogeneousPointer);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownType), 0u);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* aRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "a") aRec = &model.symbols()[i];
    ASSERT_NE(aRec, nullptr);
    ASSERT_TRUE(aRec->type.valid());
    ASSERT_EQ(ti.kind(aRec->type), TypeKind::Array);
    ASSERT_EQ(ti.scalars(aRec->type).size(), 1u);
    EXPECT_EQ(ti.scalars(aRec->type)[0], 8)
        << "sizeof(__builtin_va_list) under Win64 = sizeof(va_list) = 8";
}

TEST(SemanticAnalyzerC, SizeofBuiltinVaListMatchesVaListUnderAapcs64) {
    auto cu = buildShippedUnit("c", {
        "int a[sizeof(__builtin_va_list)];\n",
    });
    assertNoBuilderErrors(*cu);
    // Aapcs64DualCursor: __builtin_va_list = va_list = __va_list (32B struct).
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                         VaListStrategy::Aapcs64DualCursor);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownType), 0u);
    auto const& ti = model.lattice().interner();
    SymbolRecord const* aRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "a") aRec = &model.symbols()[i];
    ASSERT_NE(aRec, nullptr);
    ASSERT_TRUE(aRec->type.valid());
    ASSERT_EQ(ti.kind(aRec->type), TypeKind::Array);
    ASSERT_EQ(ti.scalars(aRec->type).size(), 1u);
    EXPECT_EQ(ti.scalars(aRec->type)[0], 32)
        << "sizeof(__builtin_va_list) under AAPCS64 = sizeof(va_list) = 32";
}

// D-CSUBSET-BUILTIN-VA-LIST-TYPE-NAME: the contract is TypeId IDENTITY, not a size
// coincidence — the injected `__builtin_va_list` SymbolRecord carries the SAME
// TypeId variable as `va_list` in every strategy branch. Identity is what makes a
// typedef chain through either name interchangeable (typedef resolution returns the
// aliased symbol's `.type` verbatim) and what routes a `__builtin_va_list`-typed
// param through the c82 va_list param-adjustment exclusion (a pure `TypeId ==`
// compare) and the va_arg isVaList shape check.
TEST(SemanticAnalyzerC, BuiltinVaListTypeIdIsIdenticalToVaListPerStrategy) {
    for (auto const strat : {VaListStrategy::SysVRegisterSave,
                             VaListStrategy::HomogeneousPointer,
                             VaListStrategy::Aapcs64DualCursor}) {
        auto cu = buildShippedUnit("c", {
            "int main(void) { return 0; }\n",
        });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                             AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                             strat);
        // The builtin scope holds exactly one record per name in this plain TU.
        SymbolRecord const* vaRec      = nullptr;
        SymbolRecord const* builtinRec = nullptr;
        for (std::size_t i = 1; i < model.symbols().size(); ++i) {
            if (model.symbols()[i].name == "va_list")
                vaRec = &model.symbols()[i];
            if (model.symbols()[i].name == "__builtin_va_list")
                builtinRec = &model.symbols()[i];
        }
        ASSERT_NE(vaRec, nullptr);
        ASSERT_NE(builtinRec, nullptr)
            << "__builtin_va_list must be injected under strategy "
            << static_cast<int>(strat);
        EXPECT_EQ(builtinRec->kind, DeclarationKind::Type);
        ASSERT_TRUE(vaRec->type.valid());
        ASSERT_TRUE(builtinRec->type.valid());
        EXPECT_EQ(builtinRec->type.v, vaRec->type.v)
            << "ONE TypeId under both names (strategy "
            << static_cast<int>(strat) << ")";
    }
}

// ★ THE DARWIN CHAIN — the REAL sqlite shape: the SDK typedefs
// `__builtin_va_list` -> `__darwin_va_list` -> `va_list`, where the LAST typedef
// REDECLARES the name `va_list` that is already bound in the builtin scope. The
// file-scope typedef legally SHADOWS the builtin-scope binding (bind() collides
// same-scope only; the tree root is a CHILD of the builtin scope) with the SAME
// TypeId, so `va_list v;` resolves through the user chain to the identical
// per-strategy type and sizeof(v) folds to the strategy's pin.
TEST(SemanticAnalyzerC, DarwinVaListTypedefChainShadowsBuiltinPerStrategy) {
    struct Row { VaListStrategy strat; int size; };
    for (auto const& row : {Row{VaListStrategy::SysVRegisterSave, 24},
                            Row{VaListStrategy::HomogeneousPointer, 8},
                            Row{VaListStrategy::Aapcs64DualCursor, 32}}) {
        auto cu = buildShippedUnit("c", {
            "typedef __builtin_va_list __darwin_va_list;\n"
            "typedef __darwin_va_list va_list;\n"
            "va_list v;\n"
            "int a[sizeof(v)];\n",
        });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                             AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                             row.strat);
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_UnknownType), 0u)
            << "every link of the Darwin chain must resolve (strategy "
            << static_cast<int>(row.strat) << ")";
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_RedeclaredSymbol), 0u)
            << "the user `va_list` typedef shadows the builtin binding, never "
               "collides";
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_NonConstantArrayLength), 0u);
        EXPECT_FALSE(model.hasErrors());
        auto const& ti = model.lattice().interner();
        SymbolRecord const* aRec = nullptr;
        for (std::size_t i = 1; i < model.symbols().size(); ++i)
            if (model.symbols()[i].name == "a") aRec = &model.symbols()[i];
        ASSERT_NE(aRec, nullptr);
        ASSERT_TRUE(aRec->type.valid());
        ASSERT_EQ(ti.kind(aRec->type), TypeKind::Array);
        ASSERT_EQ(ti.scalars(aRec->type).size(), 1u);
        EXPECT_EQ(ti.scalars(aRec->type)[0], row.size)
            << "sizeof through the Darwin chain must match the strategy pin";
    }
}

// Round-trip: a `__builtin_va_list`-typedef'd PARAMETER consumed by va_arg (the c63
// D-CSUBSET-VA-LIST-PARAM-SLOT shape under the alias name). The va_arg machinery is
// TypeId-driven — `my_va ap` carries the SAME TypeId as `va_list ap`, so isVaList
// accepts it under every strategy and the analysis is clean end-to-end.
TEST(SemanticAnalyzerC, BuiltinVaListTypedefParamRoundTripsThroughVaArg) {
    for (auto const strat : {VaListStrategy::SysVRegisterSave,
                             VaListStrategy::HomogeneousPointer,
                             VaListStrategy::Aapcs64DualCursor}) {
        auto cu = buildShippedUnit("c", {
            "typedef __builtin_va_list my_va;\n"
            "int f(int n, my_va ap) { return va_arg(ap, int); }\n",
        });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                             AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                             strat);
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_TypeMismatch), 0u)
            << "`my_va ap` must pass isVaList — same TypeId as va_list (strategy "
            << static_cast<int>(strat) << ")";
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_UnknownType), 0u);
        EXPECT_FALSE(model.hasErrors());
    }
}

// SE-pointers (G5): a pointer parameter types as Ptr in the FnSig.
TEST(SemanticAnalyzerC, PointerParamInFnSig) {
    auto cu = buildShippedUnit("c", { "void f(int *p) {}\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, GlobalArrayDeclaratorTypedAsArray) {
    auto cu = buildShippedUnit("c", { "int g[10];\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const& ti = model.lattice().interner();
    SymbolRecord const* gRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "g") gRec = &model.symbols()[i];
    ASSERT_NE(gRec, nullptr);
    ASSERT_EQ(ti.kind(gRec->type), TypeKind::Array);
    EXPECT_EQ(ti.scalars(gRec->type)[0], 10);
    EXPECT_EQ(ti.kind(ti.operands(gRec->type)[0]), TypeKind::I32);
}

// `int x;` in two DIFFERENT blocks is NOT a redecl — c's
// `block` is declared as a scope opener in the language semantics, so
// each nested block produces its own ScopeId and same-name decls are
// independent symbols.
TEST(SemanticAnalyzerC, NestedBlocksShadowWithoutRedecl) {
    auto cu = buildShippedUnit("c", {
        "int main() {\n"
        "    int x;\n"
        "    { int x; }\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "different blocks → different scopes → no shadow redecl";
    // main (function) + two distinct `x` symbols (one per block scope) + the 3
    // FC12a-core builtin TYPES (__va_list_tag + va_list + the __builtin_va_list
    // alias, D-CSUBSET-BUILTIN-VA-LIST-TYPE-NAME) + the 5 intrinsic
    // builtins (c103 __umulh + c104 _InterlockedCompareExchange + c113
    // _ReadWriteBarrier + TF-C95 __sync_synchronize (D-CSUBSET-ATOMIC-FENCE) + c115
    // _exception_code + _exception_info) + the 6
    // FC17.9(b) bit-count builtins (__builtin_{popcount,clz,ctz}{,ll},
    // D-CSUBSET-BITCOUNT-INTRINSICS) + the 56 FC17.9(b) <stdbit.h>
    // __builtin_stdc_<op>_<T> intrinsics (14 ops × 4 widths, D-FULLC-STDBIT) +
    // the 2 FC17.9(d) atomic accessors (atomic_load_explicit + atomic_store_explicit,
    // D-CSUBSET-ATOMIC) + the 4 FC17.9(f) complex builtins (__builtin_complex/creal/
    // cimag/conj, D-CSUBSET-COMPLEX) + the 6 D-CSUBSET-INTRINSIC-BSWAP byte-swap
    // builtins (_byteswap_ushort/_byteswap_ulong/_byteswap_uint64 +
    // __builtin_bswap16/32/64) + the 2 FC17.5 predefined function-name symbols
    // (__func__ + __FUNCTION__, per function definition — D-CSUBSET-FUNC-PREDEFINED-IDENTIFIER).
    EXPECT_EQ(model.symbols().size() - 1, 88u);
}

// Use-before-decl inside the same scope resolves through Pass 1's
// pre-minting (G-209 forward refs). Also asserts the use of `x` binds to
// the EXACT declared symbol AND inherits its I32 type — not just "no
// undeclared diagnostic".
TEST(SemanticAnalyzerC, ForwardReferenceWithinBlock) {
    auto cu = buildShippedUnit("c", {
        "int main() { x; int x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);

    // main (function) + x (variable) + the 3 FC12a-core builtin TYPES
    // (__va_list_tag + va_list + the __builtin_va_list alias,
    // D-CSUBSET-BUILTIN-VA-LIST-TYPE-NAME) + the 6 intrinsic builtins (c103 __umulh +
    // c104 _InterlockedCompareExchange + c113 _ReadWriteBarrier + TF-C95
    // __sync_synchronize (D-CSUBSET-ATOMIC-FENCE) + c115
    // _exception_code + _exception_info) + the 6 FC17.9(b) bit-count builtins
    // (__builtin_{popcount,clz,ctz}{,ll}, D-CSUBSET-BITCOUNT-INTRINSICS) + the 56
    // FC17.9(b) <stdbit.h> __builtin_stdc_<op>_<T> intrinsics (D-FULLC-STDBIT) +
    // the 2 FC17.9(d) atomic accessors (atomic_load_explicit + atomic_store_explicit,
    // D-CSUBSET-ATOMIC) + the 4 FC17.9(f) complex builtins (__builtin_complex/creal/
    // cimag/conj, D-CSUBSET-COMPLEX) + the 6 D-CSUBSET-INTRINSIC-BSWAP byte-swap
    // builtins (_byteswap_ushort/_byteswap_ulong/_byteswap_uint64 +
    // __builtin_bswap16/32/64) + the 2 FC17.5 predefined function-name symbols
    // (__func__ + __FUNCTION__). Find x by name.
    ASSERT_EQ(model.symbols().size() - 1, 87u);
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
TEST(SemanticAnalyzerC, LiteralsAreTyped) {
    auto cu = buildShippedUnit("c", {
        "int main() { 42; 3.14; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());

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
TEST(SemanticAnalyzerC, SameBlockRedeclEmitsError) {
    auto cu = buildShippedUnit("c", {
        "int main() { int x; int x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 1u);
}

// SE6 closure: a top-level declaration mints a symbol — variables AND
// functions. `int g = 0; int f() {...}` → two top-level symbols. With
// the `kindByChild` discriminator on `topLevelDecl`, `f` is a
// Function-kind symbol (whenRule = funcDefTail) and `g` is a
// Variable-kind symbol (the discriminator misses, so the static `kind`
// applies).
TEST(SemanticAnalyzerC, TopLevelGlobalsAndFunctionsMintSymbols) {
    auto cu = buildShippedUnit("c", {
        "int g = 0;\n"
        "int f() { return g; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ConstReassignmentEmitsConstViolation) {
    auto cu = buildShippedUnit("c", {
        "int main() { const int x = 1; x = 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_ConstViolation) EXPECT_EQ(d.actual, "x");
    }
}

// SE4: reassigning a NON-const variable → zero S_ConstViolation.
TEST(SemanticAnalyzerC, NonConstReassignmentIsClean) {
    auto cu = buildShippedUnit("c", {
        "int main() { int x = 1; x = 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
}

// D2: compound-assignment const-correctness. The c grammar's
// operator table registers every compound-assign token (`+=`, `-=`, `<<=`,
// …) as an infix operator at the same precedence/associativity as `=`, so
// `x += 2;` parses as a binaryExpr with the compound-assign token as its
// operator. Each compound-assign token has its own `assignments` entry, so
// reassigning a const through ANY of them emits S_ConstViolation exactly
// like a plain `=`.
TEST(SemanticAnalyzerC, CompoundAssignToConstEmitsConstViolation) {
    auto cu = buildShippedUnit("c", {
        "int main() { const int x = 1; x += 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, CompoundStarAssignToConstEmitsConstViolation) {
    auto cu = buildShippedUnit("c", {
        "int main() { const int x = 4; x *= 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u);
}

// D2: `<<=` against a const → exactly one S_ConstViolation (a third,
// three-char compound operator).
TEST(SemanticAnalyzerC, CompoundShlAssignToConstEmitsConstViolation) {
    auto cu = buildShippedUnit("c", {
        "int main() { const int x = 1; x <<= 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, SiblingForInitSameNameHaveDistinctScopes) {
    auto cu = buildShippedUnit("c", {
        "int main(void){\n"
        "  int s = 0;\n"
        "  for (int i = 0; i < 2; i++) s = s + i;\n"
        "  for (int i = 0; i < 2; i++) s = s + i;\n"   // same name — a distinct for-scope
        "  return s;\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "the second for(int i)'s uses must resolve to its own for-scoped i";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u)
        << "the second for(int i)'s decl must not be orphaned as unused";
}

// D-CSUBSET-FOR-INIT-SCOPE control: the for-init variable is OUT of scope AFTER the
// for-statement (it is a real for-scope, not a leak into the enclosing block). Using `i`
// after the loop must fail loud (S_UndeclaredIdentifier) — this is what proves the fix is
// a correct scope, and it stays a fail-loud reject, never a silent resolve to a stale i.
TEST(SemanticAnalyzerC, ForInitVariableOutOfScopeAfterForRejects) {
    auto cu = buildShippedUnit("c", {
        "int main(void){ for (int i = 0; i < 2; i++) {} return i; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 1u)
        << "`i` is scoped to the for-statement — a use after the loop must fail loud";
}

// D2: a NON-const variable compound-assigned (`y <<= 2;`) → zero
// S_ConstViolation. Proves the compound-assign entries gate on const-ness,
// not on the operator alone.
TEST(SemanticAnalyzerC, CompoundAssignToNonConstIsClean) {
    auto cu = buildShippedUnit("c", {
        "int main() { int y = 1; y <<= 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, MutablePointerToConstParamIsClean) {
    auto cu = buildShippedUnit("c", {
        "int f(const char *p){ p += 4; return (int)*p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
}
TEST(SemanticAnalyzerC, MutablePointerToConstEastIsClean) {
    auto cu = buildShippedUnit("c", {
        "int f(char const *p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
}

// GROUP 3 — const POINTER: the object IS const → modifying it violates.
TEST(SemanticAnalyzerC, ConstPointerParamEmitsConstViolation) {
    auto cu = buildShippedUnit("c", {
        "int f(char * const p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
}

// GROUP 4 — const pointer to const: object const → violates.
TEST(SemanticAnalyzerC, ConstPointerToConstParamEmitsConstViolation) {
    auto cu = buildShippedUnit("c", {
        "int f(const char * const p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
}

// GROUP 5 — multi-level pointers: the OUTERMOST layer decides.
// `char * const *p` — inner pointer const, OUTER pointer mutable → clean.
TEST(SemanticAnalyzerC, MultiLevelInnerConstOuterMutableIsClean) {
    auto cu = buildShippedUnit("c", {
        "int f(char * const *p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
}
// `char ** const p` — OUTER pointer const → violates.
TEST(SemanticAnalyzerC, MultiLevelOuterConstEmitsConstViolation) {
    auto cu = buildShippedUnit("c", {
        "int f(char ** const p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
}
// `const char **p` — pointee const, both pointers mutable → clean.
TEST(SemanticAnalyzerC, MultiLevelHeadConstIsClean) {
    auto cu = buildShippedUnit("c", {
        "int f(const char **p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
}

// GROUP 8 — multi-declarator: each declarator's OWN outermost layer decides.
// `const int *p, x;` → p is pointer-to-const (mutable), x is a const scalar.
TEST(SemanticAnalyzerC, MultiDeclaratorPointerCleanScalarViolates) {
    auto cu = buildShippedUnit("c", {
        "int f(){ const int *p, x; p += 1; x = 2; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    // exactly one violation — on `x` (the const scalar), NOT on `p`.
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_ConstViolation) EXPECT_EQ(d.actual, "x");
    }
}

// GROUP 9 — const + volatile together must NOT regress c27.
// `volatile char * const p` — const POINTER (volatile pointee) → violates.
TEST(SemanticAnalyzerC, VolatilePointeeConstPointerEmitsConstViolation) {
    auto cu = buildShippedUnit("c", {
        "int f(volatile char * const p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
}
// `const volatile char *p` — cv POINTEE, pointer object mutable → clean.
TEST(SemanticAnalyzerC, ConstVolatilePointeeMutablePointerIsClean) {
    auto cu = buildShippedUnit("c", {
        "int f(const volatile char *p){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
}

// GROUP 1 — scalar east-const still violates (no-pointer path unchanged).
TEST(SemanticAnalyzerC, EastConstScalarStillEmitsConstViolation) {
    auto cu = buildShippedUnit("c", {
        "int main() { int const x = 1; x = 2; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
}

// GROUP 10 — GROUPED declarators (D-CSUBSET-GROUPED-DECLARATOR-CONST closed with
// D-MIR-ELEMENT-CONST-ARRAY-GLOBAL-CLASSIFICATION): the object-forming pointer
// layer hides inside redundant parens, so declaratorObjectIsConst must DESCEND
// the group to find it. Each pin flips under revert (drop the group descent):
// the grouped const-pointer stops violating; the grouped pointer-to-const starts
// spuriously violating (the head-scan over-approximation returns).
// `char (* const p)` ≡ `char * const p` — a const POINTER object → `p += 1`
// violates. RED-ON-DISABLE: without the group descent the layer is invisible,
// the object falls to the head scan (no const in `char`) → 0 violations.
TEST(SemanticAnalyzerC, GroupedConstPointerParamEmitsConstViolation) {
    auto cu = buildShippedUnit("c", {
        "int f(char (* const p)){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u);
}
// `const char (*p)` ≡ `const char *p` — a MUTABLE pointer to const → `p += 1`
// clean. RED-ON-DISABLE: without the group descent the object falls to the head
// scan, which sees the head `const` and wrongly marks p const → a spurious
// S_ConstViolation (the pre-fix over-approximation). This is the exact
// D-CSUBSET-GROUPED-DECLARATOR-CONST closing pin (`const char (*p); p = 0;`).
TEST(SemanticAnalyzerC, GroupedPointerToConstParamIsClean) {
    auto cu = buildShippedUnit("c", {
        "int f(const char (*p)){ p += 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u);
}

// SE5: a `typedef int Foo;` mints a Type-kind alias symbol carrying the
// aliased TypeId (I32). (c's grammar parses the typedef DECL; the
// alias-in-type-position USE site is exercised generically — see the
// Synth2 typedef tests — because c's `typeBase` is keyword-only.)
TEST(SemanticAnalyzerC, TypedefMintsTypeAliasSymbol) {
    auto cu = buildShippedUnit("c", {
        "typedef int Foo;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    SymbolRecord const* fooRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "Foo") fooRec = &model.symbols()[i];
    }
    ASSERT_NE(fooRec, nullptr);
    EXPECT_EQ(fooRec->kind, DeclarationKind::Type);
    ASSERT_TRUE(fooRec->type.valid());
    EXPECT_EQ(model.lattice().interner().kind(fooRec->type), TypeKind::I32);
}

// SE6 (c): a top-level function with parameters mints a
// Function-kind symbol whose type is a FnSig with the configured param
// and return types. Driven by `topLevelDecl`'s `kindByChild` (whenRule =
// funcDefTail) + the `paramsPath: [0]` / `bodyPath: [1]` resolution into
// the funcDefTail subtree.
TEST(SemanticAnalyzerC, TopLevelFunctionBuildsFnSig) {
    auto cu = buildShippedUnit("c", {
        "int add(int a, int b) { return a; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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

// SE6 (c): a correctly-arity call to a user function → no
// diagnostic. The c call rule lives on `postfixExpr` with a
// `ParenOpen` operator-token gate (so `i++` doesn't get treated as a call).
TEST(SemanticAnalyzerC, CorrectArityCallIsClean) {
    auto cu = buildShippedUnit("c", {
        "int f(int a) { return a; }\n"
        "int main() { f(1); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotCallable), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ArgCountMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
}

// SE6 (c): too many args to a 1-arg function → exactly one
// S_ArgCountMismatch.
TEST(SemanticAnalyzerC, ExtraArgsEmitArgCountMismatch) {
    auto cu = buildShippedUnit("c", {
        "int f(int a) { return a; }\n"
        "int main() { f(1, 2, 3); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ArgCountMismatch), 1u);
}

// SE6 (c): calling a non-function (a Variable-kind global) →
// exactly one S_NotCallable.
TEST(SemanticAnalyzerC, CallingVariableEmitsNotCallable) {
    auto cu = buildShippedUnit("c", {
        "int x;\n"
        "int main() { x(1); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotCallable), 1u);
}

// SE6 (c): an UNDECLARED CALLEE in a postfix call must emit
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
TEST(SemanticAnalyzerC, UnknownCalleeEmitsExactlyOneUndeclared) {
    auto cu = buildShippedUnit("c", {
        "int main() { ggg(1); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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

// SE6 (c): `i++` is a postfix expression but NOT a call — the
// `operatorToken: ParenOpen` gate on the callRule ensures the engine
// doesn't try to call `i`. Zero S_NotCallable, zero S_ArgCountMismatch.
TEST(SemanticAnalyzerC, PostfixIncrementIsNotACall) {
    auto cu = buildShippedUnit("c", {
        "int main() { int i = 0; i++; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotCallable), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ArgCountMismatch), 0u);
}

// ── D8: unused-variable warning (warnIfUnused opt-in) ──────────────────────

// `int main(){ int unused; int used=1; return used; }` — `unused` is never
// referenced → exactly one S_UnusedVariable (a WARNING) whose `actual` is
// "unused" and whose span covers the declaration. `used` IS referenced in
// the return, so it does NOT warn.
TEST(SemanticAnalyzerC, UnusedLocalEmitsWarning) {
    auto cu = buildShippedUnit("c", {
        "int main(){ int unused; int used=1; return used; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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

// A function PARAMETER that is unused does NOT warn — c sets
// `warnIfUnused` on `varDeclHead` (locals) but NOT on `param`. This proves
// the per-declaration opt-in: same engine, same empty use-set, but no
// warning because the declaration didn't opt in.
TEST(SemanticAnalyzerC, UnusedParamDoesNotWarn) {
    auto cu = buildShippedUnit("c", {
        "int f(int unusedParam){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u)
        << "unused params are intentional — param decls do not opt in";
}

// A USED local does not warn (companion non-false-positive guard): every
// local is referenced, so zero S_UnusedVariable.
TEST(SemanticAnalyzerC, OneUnusedLocalAmongUsedWarnsOnlyForUnused) {
    auto cu = buildShippedUnit("c", {
        "int main(){ int a=1; int b=2; return a; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    // `a` is used; `b` is NOT — so exactly one warning, for `b`. Proves the
    // check fires per-symbol on the actual empty-use-set, not blanket.
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 1u);
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_UnusedVariable) EXPECT_EQ(diag.actual, "b");
    }
}

// An unused TOP-LEVEL global does NOT warn — c does not set
// `warnIfUnused` on `topLevelDecl`. Proves globals are exempt.
TEST(SemanticAnalyzerC, UnusedGlobalDoesNotWarn) {
    auto cu = buildShippedUnit("c", {
        "int unusedGlobal;\n"
        "int main(){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u)
        << "globals do not opt in to the unused-variable warning";
}

// A WRITE-ONLY local (assigned but never read) does NOT warn. The
// unused-variable scope is "never referenced" ONLY: an assignment LHS
// counts as a use, so `x = 5;` consumes `x`'s use-set even though `x`'s
// value is never read. This pins the documented scope boundary — detecting
// assigned-but-never-read is deferred to the optimizer/dataflow phase, not
// the semantic analyzer.
TEST(SemanticAnalyzerC, WriteOnlyLocalDoesNotWarn) {
    auto cu = buildShippedUnit("c", {
        "int main(){ int x; x = 5; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnusedVariable), 0u)
        << "an assignment LHS counts as a use — write-only stays for the optimizer";
}

// ── GAP A: return-type checking (returnRules facet) ────────────────────────

// `int f() { return 1; }` — I32 result, I32 literal returned → clean.
TEST(SemanticAnalyzerC, ReturnMatchingTypeIsClean) {
    auto cu = buildShippedUnit("c", {
        "int f() { return 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 0u);
}

// `int f() { return; }` — a non-Void function with a bare `return;` →
// exactly one S_ReturnTypeMismatch (a value is required).
TEST(SemanticAnalyzerC, BareReturnInNonVoidEmitsMismatch) {
    auto cu = buildShippedUnit("c", {
        "int f() { return; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 1u);
}

// `void g() { return 1; }` — a Void function returning a value → exactly
// one S_ReturnTypeMismatch. FIX 5: also pin the diagnostic's byte span — a
// value-return-in-void mismatch is emitted on the returned-VALUE node.
TEST(SemanticAnalyzerC, ValueReturnInVoidEmitsMismatch) {
    auto cu = buildShippedUnit("c", {
        "void g() { return 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, BareReturnInVoidIsClean) {
    auto cu = buildShippedUnit("c", {
        "void g() { return; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, CharParamReturnedAsIntIsClean) {
    auto cu = buildShippedUnit("c", {
        "int f(char c) { return c; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 0u)
        << "char widens to int on return (C 6.3.1.1) — the bidirectional char arm";
}

// FIX 6 companion (non-false-positive guard): returning a matching-typed
// param — `int x` returned from an `int` function — is CLEAN. Proves the
// mismatch check above is not a false positive from over-strict
// assignability (a param-use that DOES assign into the result type passes).
TEST(SemanticAnalyzerC, ReturnAssignableParamIsClean) {
    auto cu = buildShippedUnit("c", {
        "int f(int x) { return x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 0u);
}

// FIX 2 (call-result typing): a `return f();` where f returns I32 into an
// I32 function is CLEAN. Pre-fix, the call expression was never result-typed
// in pass2 while the callee identifier WAS typed with f's full FnSig, so the
// return-subtree walk surfaced the FnSig and `isAssignable(I32, FnSig)` =
// false → a spurious S_ReturnTypeMismatch. checkCall now sets the call
// node's type to the callee's result, so the walk sees I32. Zero mismatch.
TEST(SemanticAnalyzerC, ReturnOfCallResultIsClean) {
    auto cu = buildShippedUnit("c", {
        "int f() { return 1; }\n"
        "int g() { return f(); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, CharResultCallReturnedAsIntIsClean) {
    auto cu = buildShippedUnit("c", {
        "char h() { return 'a'; }\n"
        "int g() { return h(); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 0u)
        << "a Char-result call widens to int on return (C 6.3.1.1)";
}

// R2 (sizeof char/string fold cycle): a CHARACTER constant has type `int`
// (C 6.4.4.4 — the reason `sizeof('c')`==4, not 1). Pinned in a context where the
// int type MATTERS: `f('c')` to an `int*` param fires a mismatch (int 99 is not a
// pointer, and not the null constant 0). RED-ON-DISABLE: drop the CharLiteral→I32
// `literalTypes` row → `'c'` is untyped → `isAssignable` short-circuits on
// InvalidType → 0 mismatch (the literal would silently pass).
TEST(SemanticAnalyzerC, CharLiteralIsTypedIntNotUntyped) {
    auto cu = buildShippedUnit("c", {
        "extern void f(int* p);\n"
        "int main() { f('c'); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
        << "'c' has type int (C 6.4.4.4) → passing it to an int* param is a mismatch";
}

// R2: a STRING literal has type `char[N+1]` (C 6.4.5 — the reason `sizeof("abcd")`
// ==5). Pinned where the ELEMENT type matters: passing "abc" to an `int*` param
// fires a mismatch (Array<Char>→Ptr<int> fails the same-element-type array-decay
// rule), while to a `char*` param it decays cleanly (0 — covered by the existing
// string corpus). RED-ON-DISABLE: drop the StringLiteral `stringArray` row → "abc"
// is untyped → `isAssignable` short-circuits → 0 mismatch.
TEST(SemanticAnalyzerC, StringLiteralIsTypedCharArrayNotUntyped) {
    auto cu = buildShippedUnit("c", {
        "extern void g(int* p);\n"
        "int main() { g(\"abc\"); return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, AdjacentStringConcatTypesWholeCharArray) {
    auto cu = buildShippedUnit("c", {
        "int main() { \"hello\" \" world\"; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ConcatEffectivePrefixTypesWholeWideArray) {
    auto cu = buildShippedUnit("c", { "void f(){ \"a\" L\"b\"; }" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ConcatSamePrefixTypesU16) {
    auto cu = buildShippedUnit("c", { "void f(){ u\"a\" u\"b\"; }" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ConcatConflictLeavesNodeUntypedAndEmits) {
    auto cu = buildShippedUnit("c", { "void f(){ u\"a\" U\"b\"; }" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_TRUE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::H_ConflictingStringLiteralPrefixes), 1u);
    TypeId const ty = firstStringLiteralType(model, *cu);
    EXPECT_FALSE(ty.valid())
        << "a mixed-prefix run is left UNTYPED so a sizeof of it fails loud";
}

// N6: `sizeof(u"a" U"b")` fails loud with the conflict reason (not a silent fold /
// bare sizeof-of-untyped). The conflict is emitted once, on the rule node.
TEST(SemanticAnalyzerC, ConcatConflictSizeofFailsLoud) {
    auto cu = buildShippedUnit("c", { "int f(){ return sizeof(u\"a\" U\"b\"); }" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ConcatConflictIsTokenKindNotCoreEvenOnPe) {
    for (bool pe : {false, true}) {
        auto cu = buildShippedUnit("c", { "void f(){ u\"a\" L\"b\"; }" });
        assertNoBuilderErrors(*cu);
        auto model = pe ? analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Llp64, std::nullopt, std::nullopt,
                                  ObjectFormatKind::Pe)
                        : analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, CharLiteralInitializesCharSlotCleanly) {
    auto cu = buildShippedUnit("c", {
        "int main() { char x = 'c'; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "an int literal initializing a char slot is legal C (the char↔int arm)";
}

// FIX 3 (crit-8, nested return): a value `return 1;` nested inside an `if`
// inside a `void` function body still checks against the function's result
// type — exactly one S_ReturnTypeMismatch. Proves checkReturn's scope-
// parent-chain walk reaches the enclosing function across the intermediate
// `if`-block scope.
TEST(SemanticAnalyzerC, NestedReturnChecksAgainstEnclosingFunction) {
    auto cu = buildShippedUnit("c", {
        "void g() { if (1) { return 1; } }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 1u)
        << "a nested value-return in a void function must reach the enclosing result";
}

// ── GAP B: duplicate parameter names ───────────────────────────────────────

// `int f(int x, int x) {}` — two params named `x` bind into the same
// (funcDefTail) scope, so the second collides → exactly one
// S_RedeclaredSymbol with a RelatedLocation to the first param.
TEST(SemanticAnalyzerC, DuplicateParamNamesEmitRedecl) {
    auto cu = buildShippedUnit("c", {
        "int f(int x, int x) { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, FnPtrStructMembersSharedParamNameNoRedecl) {
    auto model = analyzeShipped("c", {
        "struct M { int (*a)(int v); int (*b)(int v); };\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "sibling fn-ptr members sharing a param name must not collide "
           "(per-declarator function-prototype scope)";
}

// (2) fn-ptr TYPEDEFS with a shared param name → no collision.
TEST(SemanticAnalyzerC, FnPtrTypedefsSharedParamNameNoRedecl) {
    auto model = analyzeShipped("c", {
        "typedef int (*A)(int x);\n"
        "typedef int (*B)(int x);\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "two fn-ptr typedefs sharing a param name must not collide";
}

// (3) fn-ptr PARAMS (of an ordinary function) with a shared param name → no
// collision. `void h(int (*a)(int x), int (*b)(int x));`
TEST(SemanticAnalyzerC, FnPtrParamsSharedParamNameNoRedecl) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FnPtrParamNameDoesNotLeakToEnclosingScope) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FnPtrParamNameDoesNotResolveOutsideDeclarator) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, NestedFnPtrParamsSharedNamesNoRedecl) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, DefinitionParamsRemainBodyVisible) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, PlainPrototypesSharedParamNameClean) {
    auto model = analyzeShipped("c", {
        "int f(int x);\n"
        "int g(int x);\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u);
}

// (8) PRESERVE the genuine-duplicate error: two params named `x` in ONE
// declarator still collide (they share the SAME prototype scope), in a fn-ptr
// member too — the isolation is PER-DECLARATOR, not per-param. (The function-
// DEFINITION form is pinned by DuplicateParamNamesEmitRedecl above.)
TEST(SemanticAnalyzerC, DuplicateParamNameInSingleFnPtrStillCollides) {
    auto model = analyzeShipped("c", {
        "struct M { int (*a)(int x, int x); };\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "a genuine duplicate within ONE declarator's param list must still error";
}

// ── GAP C: break/continue outside loop (loopControls facet) ────────────────

// `while (1) { break; }` — a break inside a loop body → clean.
TEST(SemanticAnalyzerC, BreakInsideLoopIsClean) {
    auto cu = buildShippedUnit("c", {
        "int main() { while (1) { break; } return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ControlOutsideLoop), 0u);
}

// A bare `break;` in a function body but outside any loop → exactly one
// S_ControlOutsideLoop. FIX 5: also pin the diagnostic's exact byte span.
TEST(SemanticAnalyzerC, BreakOutsideLoopEmitsDiagnostic) {
    auto cu = buildShippedUnit("c", {
        "int main() { break; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
// c's `switchStmt` is a configured loopRules (break-context) entry,
// so a break there is NOT outside-loop.
TEST(SemanticAnalyzerC, BreakInsideSwitchIsClean) {
    auto cu = buildShippedUnit("c", {
        "int main(){ switch(1){ case 1: break; } return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ControlOutsideLoop), 0u)
        << "switch is a configured break-context";
}

// FIX 4 (nested-loop depth): a break in an inner loop nested in an outer
// loop is clean (depth ≥ 1 throughout the inner body).
TEST(SemanticAnalyzerC, NestedLoopBreakIsClean) {
    auto cu = buildShippedUnit("c", {
        "int main(){ while(1){ while(1){ break; } } return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ControlOutsideLoop), 0u);
}

// FIX 4 (depth decrement after a loop closes): a break placed AFTER a loop
// body has closed is back at depth 0 → exactly one S_ControlOutsideLoop.
// Proves the loop-depth increment is scoped to the loop subtree and is
// correctly NOT carried past the loop's closing brace.
TEST(SemanticAnalyzerC, BreakAfterLoopIsOutsideLoop) {
    auto cu = buildShippedUnit("c", {
        "int main(){ while(1){ } break; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ControlOutsideLoop), 1u)
        << "a break after the loop closes is back at depth 0";
}

// ── GAP F (FC13): split `# include` ───────────────────────────────────────

// `# include "x.h"` (whitespace between `#` and `include`) is recognized by
// the config-selected preprocessor (its directive scan skips trivia between
// the intro `#` and the `include` word) and the header is INLINED. This both
// closes GAP F and proves the FC13 splice handles a spaced directive: one
// tree, zero cross-refs, header text present.
TEST(SemanticAnalyzerC, SpacedIncludeIsInlined) {
    // D-TEST-FIXED-SCRATCH-PATH-POPULATION — the one site in this file that had
    // drifted: the include dir came from a CONSTANT name under
    // `temp_directory_path()`, so two concurrent instances of this binary shared
    // it and the first one's `remove_all` deleted `x.h` while the second was
    // preprocessing (MEASURED: "cannot open .../dss_gapF_include_test/x.h" and
    // "quote include not found: x.h"). Every other scratch site in this file
    // already uses `ScratchDir`, where the pid is only a SEED and the actual
    // guarantee is the atomic claim — SINGULAR `create_directory`, which returns
    // true only for the caller that created the slot — so this one now matches
    // its neighbours. `dss::test_support::` is spelled out because the `using`
    // declarations for it appear further down, in the FF11 block.
    dss::test_support::ScratchDir incDir{
        dss::test_support::Location::Temp, "gapF-include"};
    {
        std::ofstream(incDir.path() / "x.h", std::ios::binary)
            << "int helper() { return 1; }\n";
    }
    auto schema = loadShippedSchema("c");
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addIncludeDir(incDir.path());
    builder.addInMemory("# include \"x.h\"\nint main() { return helper(); }\n", "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    EXPECT_EQ(cu->trees().size(), 1u);
    EXPECT_TRUE(cu->crossRefs().empty());
    EXPECT_FALSE(hasCode(cu->driverDiagnostics(), DiagnosticCode::D_UnresolvedImport));
    EXPECT_NE(std::string{cu->trees()[0].source().text()}.find("int helper()"),
              std::string::npos)
        << "a spaced `# include` must still be recognized + inlined";
    // No manual `remove_all` — `incDir`'s dtor owns the cleanup (and warns loudly
    // on stderr if it cannot, so a leak stays visible).
}

// ── FF11: angle-include resolves a NEUTRAL JSON DESCRIPTOR + injects its ──────
// ── externs at the SEMANTIC phase + GOAL-2 (user decl wins) ──────────────────
//
// These pin the DESCRIPTOR model (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC) that
// REPLACED cycle-21's source-`.h` load. An angle `#include <io.h>` no longer
// pulls in a parsed c header; it resolves to `io.json` on the system
// dir, and the semantic phase reads that descriptor and MINTS its externs into
// scope BEFORE Pass 2 (the builtinFunctions analogue) — so a call resolves.
// A descriptor symbol a USER declaration already claims is SKIPPED (goal-2:
// user decl wins; no duplicate symbol). The old cycle-21 tree-level
// S_RedeclaredSymbol no longer applies (a descriptor is not a tree).

namespace {
using dss::test_support::Location;
using dss::test_support::ScratchDir;

// Build a c CU whose `main.c` source is `mainSrc`, with a NEUTRAL JSON
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
    auto schema = loadShippedSchema("c");
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
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
TEST(SemanticAnalyzerC, FF11AngleIncludeResolvesPutsViaDescriptor) {
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

    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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

// Per-SYMBOL `library` OVERRIDE (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC): a symbol
// that carries its own `library` map has THAT map MERGED OVER the descriptor's at
// injection (symbol keys WIN; a format the symbol OMITS inherits the descriptor's
// entry), while a SIBLING with no override binds the descriptor default unchanged.
// This is the mechanism the pe `strftime`->ucrtbase.dll date4 fix uses — bind ONE
// symbol to a different image than its header default without moving the whole
// descriptor. RED-ON-DISABLE: revert the injector to pass `desc->library` instead
// of the merged `effectiveLibrary`, and the override symbol's `pe` entry reverts
// to the descriptor default (msvcrt.dll) → the `over->library.at("pe")` assertion
// flips from "ucrtbase.dll" to "msvcrt.dll".
TEST(SemanticAnalyzerC, ShippedPerSymbolLibraryOverrideMergesOverDescriptor) {
    ScratchDir sysDir{Location::Temp, "ff11-symlib"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "tt.json",
        R"JSON({ "header": "tt.h", "library": { "pe": "msvcrt.dll", "elf": "libc.so.6" },
             "symbols": [
                 { "name": "over",  "signature": "fn() -> i32", "library": { "pe": "ucrtbase.dll" } },
                 { "name": "plain", "signature": "fn() -> i32" } ] })JSON",
        "#include <tt.h>\nint main() { return over() + plain(); }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "both descriptor symbols must resolve";

    ASSERT_EQ(model.shippedExterns().size(), 2u);
    ShippedExternSymbol const* over  = nullptr;
    ShippedExternSymbol const* plain = nullptr;
    for (auto const& ext : model.shippedExterns()) {
        if (ext.name == "over")  over  = &ext;
        if (ext.name == "plain") plain = &ext;
    }
    ASSERT_NE(over,  nullptr);
    ASSERT_NE(plain, nullptr);
    // The OVERRIDE symbol: `pe` is the override image (ucrtbase.dll — symbol wins),
    // `elf` INHERITS the descriptor default (libc.so.6 — the omitted-key semantics,
    // NOT dropped). Both entries present → the merge, not a replace.
    ASSERT_EQ(over->library.size(), 2u);
    EXPECT_EQ(over->library.at("pe"),  "ucrtbase.dll");
    EXPECT_EQ(over->library.at("elf"), "libc.so.6");
    // The SIBLING with no override binds the descriptor default on EVERY format —
    // byte-identical to the pre-override behavior.
    ASSERT_EQ(plain->library.size(), 2u);
    EXPECT_EQ(plain->library.at("pe"),  "msvcrt.dll");
    EXPECT_EQ(plain->library.at("elf"), "libc.so.6");
}

// ── Item 1: shipped-header CONSTANTS + TYPEDEFS via the neutral descriptor ────

// A shipped CONSTANT injects + folds in CONSTANT-EXPRESSION position (an array
// dimension) — the const-eval direct-value arm (MF-1). The descriptor CHAR_BIT
// (=8) makes `int a[CHAR_BIT]` a valid 8-element array. RED-ON-DISABLE: remove
// the const-eval `resolveSymbolValue` arm and `int a[CHAR_BIT]` fails loud with
// S_NonConstantArrayLength (an injected constant has no init-CST to walk).
TEST(SemanticAnalyzerC, ShippedConstantFoldsInArrayDimension) {
    ScratchDir sysDir{Location::Temp, "item1-const"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "limits.json",
        R"({ "header": "limits.h",
             "constants": [ { "name": "CHAR_BIT", "value": 8, "type": "i32" } ] })",
        "#include <limits.h>\nint main() { int a[CHAR_BIT]; return 0; }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, ShippedTypedefResolvesInTypePosition) {
    ScratchDir sysDir{Location::Temp, "item1-typedef"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "mytypes.json",
        R"({ "header": "mytypes.h",
             "typedefs": [ { "name": "my_int_t", "type": "i32" } ] })",
        "#include <mytypes.h>\nint main() { my_int_t x; x = 5; return x; }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u)
        << "my_int_t must resolve via the injected descriptor typedef";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
}

// C34b (D-FFI-DESCRIPTOR-UNION-MEMBER-INJECTION): a shipped struct with a
// UNION-typed field → `e.key.member` resolves to the member type. The union's
// member NAMES get a `compositeScopeByType` field scope injected (the NEW mechanism
// mirroring struct-field injection), so the nested `structValue.unionField.member`
// access resolves — the exact shape of the real `Tcl_GetHashKey` macro's
// `h->key.oneWordValue` / `h->key.string`. RED-ON-DISABLE: remove the `desc->unions`
// injection loop in semantic_analyzer.cpp and `e.key.oneWordValue` fails
// S_NotAComposite (the union value has no member scope).
TEST(SemanticAnalyzerC, ShippedUnionFieldMemberResolves) {
    ScratchDir sysDir{Location::Temp, "c34b-union"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "hk.json",
        R"JSON({ "header": "hk.h",
             "typedefs": [
                 { "name": "KeyU", "type": "union \"KeyU\" { ptr<void>, ptr<char> }" },
                 { "name": "Ent",  "type": "struct \"Ent\" { ptr<void>, KeyU }" } ],
             "unions": [ { "name": "KeyU", "fields": [
                 { "name": "oneWordValue", "type": "ptr<void>" },
                 { "name": "str",          "type": "ptr<char>" } ] } ],
             "structs": [ { "name": "Ent", "fields": [
                 { "name": "cd",  "type": "ptr<void>" },
                 { "name": "key", "type": "KeyU" } ] } ] })JSON",
        "#include <hk.h>\n"
        "int main(void) { Ent e; void *p = e.key.oneWordValue; char *q = e.key.str;"
        " return (p != 0) + (q != 0); }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u)
        << "e.key (a union value) must have a member scope so .oneWordValue resolves";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "oneWordValue/str resolve as union members";
}

// The fail-loud pin: an UNKNOWN union member is rejected. The union member scope IS
// registered (so this is NOT a non-composite miss), but the member name is absent →
// S_UndeclaredIdentifier, never a silent wrong-but-runs access.
TEST(SemanticAnalyzerC, ShippedUnknownUnionMemberFailsLoud) {
    ScratchDir sysDir{Location::Temp, "c34b-union-bad"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "hk2.json",
        R"JSON({ "header": "hk2.h",
             "typedefs": [
                 { "name": "KeyU", "type": "union \"KeyU\" { ptr<void>, ptr<char> }" } ],
             "unions": [ { "name": "KeyU", "fields": [
                 { "name": "oneWordValue", "type": "ptr<void>" },
                 { "name": "str",          "type": "ptr<char>" } ] } ] })JSON",
        "#include <hk2.h>\n"
        "int main(void) { KeyU u; void *p = u.noSuchMember; return p != 0; }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u)
        << "KeyU IS a composite (union member scope registered) — not a non-composite miss";
    EXPECT_GE(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 1u)
        << "an unknown union member must fail loud";
}

// GOAL-2: a user decl of a name WINS over a descriptor constant of the same
// name — and the skip is SELECTIVE (a different descriptor constant the user
// does NOT declare is still injected). The descriptor declares CHAR_BIT
// (user-overridden) + WIDTH (injected). RED if it skips nothing (CHAR_BIT
// doubled) AND RED if it skips everything (WIDTH lost → S_UndeclaredIdentifier).
TEST(SemanticAnalyzerC, ShippedConstantUserDeclWins) {
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
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countSymbolsNamed(model, "CHAR_BIT"), 1u)
        << "the user's CHAR_BIT wins; the descriptor's is skipped (no double-bind)";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "WIDTH (not user-declared) must still inject + resolve";
    EXPECT_FALSE(hasCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol));
}

// MF-3: a shipped constant is `isConst` — writing to it emits S_ConstViolation
// (a macro constant is not assignable), and the InvalidTree / no-declRuleNode
// symbol does NOT crash the const-violation path.
TEST(SemanticAnalyzerC, WriteToShippedConstantViolatesConst) {
    ScratchDir sysDir{Location::Temp, "item1-constviol"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "limits.json",
        R"({ "header": "limits.h",
             "constants": [ { "name": "CHAR_BIT", "value": 8, "type": "i32" } ] })",
        "#include <limits.h>\nint main() { CHAR_BIT = 5; return 0; }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, FF11AngleIncludePlusInlineExternUserDeclWins) {
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

    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, FF11SameDescriptorIncludedTwiceInjectsOnce) {
    ScratchDir sysDir{Location::Temp, "ff11-desc"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "io.json",
        R"({ "header": "io.h", "library": { "pe": "msvcrt.dll", "elf": "libc.so.6" },
             "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ] })",
        "#include <io.h>\n"
        "#include <io.h>\n"
        "int main() { puts(\"hi\"); return 0; }\n");
    ASSERT_EQ(cu->trees().size(), 1u);
    // Two directives resolving to the SAME descriptor → ONE recorded path
    // (D-FFI-DESCRIPTOR-INCLUDES: the import resolver's CU-wide closure visited-set
    // dedups at RECORD time — a descriptor reached twice, whether transitively or,
    // as here, directly-included-twice, is recorded once). The semantic injection
    // loop's own canonical-path `readDescriptors` dedup already collapsed the
    // second ref pre-change, so injections + diagnostics are byte-identical; only
    // the redundant ref is no longer stored. RED-ON-DISABLE of the whole path stays
    // the injection assertions below (puts minted exactly once).
    EXPECT_EQ(cu->shippedLibDescriptors().size(), 1u);
    assertNoBuilderErrors(*cu);

    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, FF11MultipleDescriptorsEachSymbolInjectedOnce) {
    ScratchDir sysDir{Location::Temp, "ff11-desc-multi"};
    auto writeDesc = [&](std::string const& stem, std::string const& sym) {
        std::ofstream(sysDir.path() / (stem + ".json"), std::ios::binary)
            << R"({ "header": ")" << stem << R"(.h", "library": { "pe": "msvcrt.dll" }, )"
            << R"("symbols": [ { "name": ")"
            << sym << R"(", "signature": "fn() -> i32" } ] })";
    };
    writeDesc("dup", "dup");
    for (int i = 0; i < 4; ++i) writeDesc("s" + std::to_string(i), "s" + std::to_string(i));

    auto schema = loadShippedSchema("c");
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
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
    // Six directives, but `dup` is included TWICE → FIVE distinct recorded paths
    // (D-FFI-DESCRIPTOR-INCLUDES: the import resolver's CU-wide closure visited-set
    // dedups the repeated `dup` at RECORD time; the semantic loop's canonical-path
    // dedup already collapsed it pre-change, so the injection result below is
    // byte-identical — only the redundant `dup` ref is no longer stored).
    EXPECT_EQ(cu->shippedLibDescriptors().size(), 5u);
    assertNoBuilderErrors(*cu);

    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
// made float->int an ADMITTED implicit conversion in c, so the implicit
// form no longer fires; a distinct-typed-pointer pair is the stable implicit-
// rejected / explicit-accepted contrast that still exercises the same FC2
// explicit-cast-vs-implicit-assignability split.)
TEST(SemanticAnalyzerC, ExplicitPointerCastAcceptedWhereImplicitRejected) {
    auto implicitModel = analyzeShipped("c", {
        "int* f(char* p) { return p; }\n",
    });
    EXPECT_EQ(countCode(implicitModel.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 1u)
        << "the implicit char* -> int* conversion must stay rejected";

    auto castModel = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, TypedefNameInCastPositionResolves) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, UnknownTypeNameInCommittedCastFiresUnknownType) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, StructValueCastsAreRejected) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FunctionDesignatorToFnPtrCastIsAccepted) {
    auto model = analyzeShipped("c", {
        "typedef void(*dtor)(void*);\n"
        "void real(void* p){ (void)p; }\n"
        "int main(){ dtor d = (dtor)real; return d ? 0 : 1; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidCast), 0u)
        << "a function designator cast to its own fn-ptr type is legal C";
}

// Positive — CROSS-signature fn-ptr cast (C 6.3.2.3p8; sqlite syscall shapes).
TEST(SemanticAnalyzerC, CrossSignatureFnPtrCastIsAccepted) {
    auto model = analyzeShipped("c", {
        "typedef int(*fp)(int);\n"
        "void g(void* x){ (void)x; }\n"
        "int main(){ fp f = (fp)g; return f ? 0 : 1; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidCast), 0u)
        << "cross-signature fn-ptr cast is legal C (calling through it is UB, the cast is not)";
}

// Negative (over-admission guard) — STRUCT value -> fn-ptr stays REJECTED.
TEST(SemanticAnalyzerC, StructToFnPtrCastStillRejected) {
    auto model = analyzeShipped("c", {
        "typedef void(*fp)(void);\n"
        "struct S { int x; };\n"
        "int main(){ struct S s; s.x = 0; fp f = (fp)s; return f ? 0 : 1; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidCast), 1u)
        << "a struct value is not a function designator — must stay fail-loud";
}

// Negative (over-admission guard) — function designator -> INT stays REJECTED
// (the new arm is pointer-target-only: tk==Ptr).
TEST(SemanticAnalyzerC, FunctionDesignatorToIntStillRejected) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, NestedTagReferencedAtFileScopeComposes) {
    auto model = analyzeShipped("c", {
        "struct Outer { struct Inner { int x; } m; };\n"
        "int f(struct Inner *p){ return p->x; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_IncompleteTypeObject), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u);
}

// c38b — accept: a value object of the nested tag works (it is COMPLETE at file scope).
TEST(SemanticAnalyzerC, NestedTagValueObjectWorks) {
    auto model = analyzeShipped("c", {
        "struct Outer { struct Inner { int x; } m; };\n"
        "int main(void){ struct Inner v; v.x = 5; return v.x; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_IncompleteTypeObject), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u);
}

// c38c — REGRESSION guard: member composition (`o->m.x`) must STILL work
// (the nested struct's member TYPE is resolved via structScope, independent of
// the tag BIND scope — the fix must not break this).
TEST(SemanticAnalyzerC, NestedTagMemberAccessStillComposes) {
    auto model = analyzeShipped("c", {
        "struct Outer { struct Inner { int x; } m; };\n"
        "int g(struct Outer *o){ return o->m.x; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u);
}

// c38d — OVER-FLOAT guard: a nested tag in a BLOCK-local struct floats only to
// the BLOCK scope, NOT file scope — a file-scope reference must STILL fail.
TEST(SemanticAnalyzerC, NestedTagInBlockDoesNotLeakToFileScope) {
    auto model = analyzeShipped("c", {
        "void f(void){ struct Outer { struct Inner { int x; } m; }; }\n"
        "int main(void){ struct Inner v; v.x = 0; return v.x; }\n",
    });
    // Inner is block-scoped to f's body; at file scope it is unknown/incomplete.
    EXPECT_GE(countCode(model.diagnostics(), DiagnosticCode::S_IncompleteTypeObject)
              + countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 1u);
}

// c38e — union nested tag at file scope.
TEST(SemanticAnalyzerC, NestedUnionTagComposes) {
    auto model = analyzeShipped("c", {
        "union U { struct Item { int v; } it; };\n"
        "int f(struct Item *p){ return p->v; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_IncompleteTypeObject), 0u);
}

// c38f — deeply nested (struct in struct in struct): the innermost tag floats
// all the way to file scope.
TEST(SemanticAnalyzerC, DeeplyNestedTagComposes) {
    auto model = analyzeShipped("c", {
        "struct A { struct B { struct C { int v; } c; } b; };\n"
        "int f(struct C *p){ return p->v; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_IncompleteTypeObject), 0u);
}

// c38g — shadowing: a nested tag of the same name as a FILE-scope tag both land
// in file scope → a redefinition collision (C 6.7p3 — one definition per scope).
TEST(SemanticAnalyzerC, NestedTagShadowingFileScopeTagCollides) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, PointerSubtractionAsCallArgIsClean) {
    auto model = analyzeShipped("c", {
        "void g(long x){ (void)x; }\n"
        "int f(char* a, char* b){ g(a - b); return 0; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}

// c40b — int* p-q also passes as a numeric arg (the type is I64 regardless of pointee).
TEST(SemanticAnalyzerC, PointerSubtractionIntPtrAsCallArgIsClean) {
    auto model = analyzeShipped("c", {
        "void g(long x){ (void)x; }\n"
        "int f(int* a, int* b){ g(a - b); return 0; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}

// c40c — p-q returned as long (the result type is ptrdiff_t) + assigned.
TEST(SemanticAnalyzerC, PointerSubtractionReturnAndAssignIsClean) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, MismatchedPointeeSubtractionRejectedAsNumericArg) {
    auto model = analyzeShipped("c", {
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

TEST(SemanticAnalyzerC, PointerPlusIntIsCleanPointerTyped) {
    auto model = analyzeShipped("c", {
        "int f(int* p, int n){ int* q = p + n; return *q; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}
// The key commutative case: `n + p` must type as a pointer, not the integer.
TEST(SemanticAnalyzerC, IntPlusPointerIsCleanPointerTyped) {
    auto model = analyzeShipped("c", {
        "int f(int* p, int n){ int* q = n + p; return *q; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}
TEST(SemanticAnalyzerC, PointerMinusIntIsCleanPointerTyped) {
    auto model = analyzeShipped("c", {
        "int f(int* p, int n){ int* q = p - n; return *q; }\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}
// GUARD: `int + int` is NOT pointer arithmetic (the Ptr guard is not over-broad).
TEST(SemanticAnalyzerC, IntPlusIntUnaffectedByPtrArith) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AbstractFnPtrCastTypesAsPtrToFnVoidInt) {
    auto cu = buildShippedUnit("c", {
        "int main() { void* p; return ((int(*)(void))p) != 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, AbstractFnPtrCastWithParamTypesCorrectly) {
    auto cu = buildShippedUnit("c", {
        "int main() { void* p; return ((int(*)(int))p) != 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, SizeofAbstractFnPtrResolvesCleanly) {
    auto cu = buildShippedUnit("c", {
        "int main() { return (int)sizeof(int(*)(void)); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
}

// FAIL-LOUD: a NAMED declarator in a type-name position (`(int x)p`) is a C
// constraint violation (type-names are abstract) — S_TypeNameDeclaratorNotAbstract,
// never silently parsed as `(int)`. This is the inverse of
// S_DeclarationDeclaresNothing.
TEST(SemanticAnalyzerC, NamedCastDeclaratorFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AbstractFnPtrCastUnknownBaseStillUnknownType) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, PostStarConstCastStripsToPlainPointer) {
    auto cu = buildShippedUnit("c", {
        "int main() { int* p;\n"
        "  int* a = (int * const)p;\n"
        "  int* b = (int *)p;\n"
        "  return (a == b); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, PostStarVolatileCastStripsPointerVolatile) {
    auto cu = buildShippedUnit("c", {
        "typedef unsigned int u32;\n"
        "int main() { void* p; return ((u32 * volatile)p) != 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, PreStarVolatileCastKeepsPointeeVolatile) {
    auto cu = buildShippedUnit("c", {
        "typedef unsigned int u32;\n"
        "int main() { void* p; return ((volatile u32 *)p) != 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, PreStarVolatileDoublePtrCastBuildsNestedPointee) {
    auto cu = buildShippedUnit("c", {
        "typedef unsigned int u32;\n"
        "int main() { void* p; return ((volatile u32 **)p) != 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, AtomicQualifierResolvesAtomicQualified) {
    auto cu = buildShippedUnit("c", {
        "_Atomic int x;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, AtomicOnAggregateFailsLoudNonLockFree) {
    auto cu = buildShippedUnit("c", {
        "struct S { int a; int b; };\n"
        "_Atomic struct S x;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AtomicNonLockFree), 1u)
        << "_Atomic on an aggregate must fail loud (non-lock-free — deferred)";
}

// The NEGATIVE that keeps the gate honest: a lock-free SCALAR `_Atomic` must NOT be
// rejected (it is the supported case — it resolves to atomicQualified + lowers to the
// atomic opcodes). Without it, a too-broad reject would silently break scalar atomics.
TEST(SemanticAnalyzerC, AtomicOnScalarNotRejectedNonLockFree) {
    auto cu = buildShippedUnit("c", {
        "_Atomic int x;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AtomicNonLockFree), 0u)
        << "a lock-free scalar _Atomic must be accepted (the supported case)";
}

// ★ D-CSUBSET-UINT128-TYPE (TF-C94): `_Atomic __int128` / `_Atomic unsigned __int128`
// must FAIL LOUD with the SAME S_AtomicNonLockFree the aggregate sibling above emits.
// This is not a new rule — it is the generalization of `isByValueClass` reaching the
// 128-bit kinds. A 128-bit integer is MEMORY-RESIDENT (multi-limb, reached by ADDRESS),
// so `_Atomic` on one has exactly the aggregate's problem: the qualifier is a
// TRANSPARENT skin, the wrapped value reaches codegen, the limb emitters decompose it
// into per-limb Load/Store, and the result is a SILENT non-atomic access that no
// type-based belt can see. `_BitInt(128)` is included as the third row — the SAME
// predicate decides all three, so if one is admitted they all are.
//
// ★ THE MEASURED ASYMMETRY, recorded because it looks like a hole and is not:
// `_Atomic long double` compiles with ZERO diagnostics on this same build. That is
// CORRECT under the current predicate, not an oversight — `isByValueClass` is the gate,
// and F80/F128 are NOT `isByValueClass` (they are scalars in one FPR/x87 slot, not
// multi-limb memory-resident values), so a `long double` never reaches the reject.
// MEASURED, arm64:macho64-arm64-darwin-exec, one file per row:
//     `_Atomic __int128 x;`               → 1 × S0055 (S_AtomicNonLockFree)
//     `_Atomic unsigned __int128 y;`      → 1 × S0055
//     `_Atomic unsigned _BitInt(128) w;`  → 1 × S0055
//     `_Atomic long double z;`            → 0 diagnostics
//     `_Atomic long long q;`              → 0 diagnostics
// Whether a 16-byte `long double` genuinely IS lock-free is a separate question owned by
// the long-double arc (D-CSUBSET-LONG-DOUBLE); it is NOT silently in scope here, and
// changing it must be a deliberate edit to `isByValueClass`'s membership, not a
// side-effect. Recorded so a future reader does not "fix" the asymmetry by accident.
TEST(SemanticAnalyzerC, AtomicOnWideIntegerFailsLoudNonLockFree) {
    struct Row { char const* src; char const* what; };
    for (Row const r : {
             Row{"_Atomic __int128 x;\n",              "_Atomic __int128"},
             Row{"_Atomic unsigned __int128 y;\n",     "_Atomic unsigned __int128"},
             Row{"_Atomic unsigned _BitInt(128) w;\n", "_Atomic unsigned _BitInt(128)"}}) {
        auto cu = buildShippedUnit("c", {std::string{r.src}});
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_AtomicNonLockFree), 1u)
            << "\n" << r.what << " is memory-resident (multi-limb) — _Atomic on it must "
               "fail loud, exactly as _Atomic on an aggregate does";
    }
}

// The NEGATIVE control that pins the MEASURED asymmetry above as a deliberate boundary
// rather than an accident: a 16-byte `long double` is NOT `isByValueClass`, so it does
// NOT trip the wide-integer reject. If a future edit broadened the gate from
// "by-value class" to "bigger than a register", THIS is what would turn red first —
// and it should, because that would be a real scope change to the long-double arc.
TEST(SemanticAnalyzerC, AtomicOnLongDoubleNotRejectedNonLockFree) {
    auto cu = buildShippedUnit("c", {
        "_Atomic long double z;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AtomicNonLockFree), 0u)
        << "MEASURED: _Atomic long double compiles with ZERO diagnostics — F80/F128 are "
           "not isByValueClass, so the wide-integer reject must NOT reach them";
}

// ★★ D-CSUBSET-INT128-CONSTFOLD (TF-C94) — a 128-bit INTEGER CONSTANT EXPRESSION.
//
// THE DEFECT THIS PINS. `cst_const_eval.cpp`'s Cast fold gained a 128-bit arm that
// routes `(__int128)`/`(__uint128_t)` casts through the bignum instead of narrowing
// them into an int64. It shipped DEAD: `semantic_analyzer.cpp`'s `resolveCastTarget`
// had no I128/U128 row, so a 128-bit cast target fell to its `default: nullopt`, the
// whole cast was non-foldable, and EVERY 128-bit integer constant expression refused —
// not merely the wide ones. MEASURED before the fix (arm64:macho64-arm64-darwin-exec,
// one file per row), each S0029 "static assertion condition is not an integer constant
// expression":  `(__uint128_t)5 == 5`, `((__uint128_t)5 + 1) == 6`,
// `(int)((__uint128_t)5) == 5`. Plain `5 == 5` was clean, and the `_BitInt(128)` twin
// of the first row was ALREADY clean — that asymmetry is the whole bug.
//
// ★★ WHY THE DEFECT SHIPPED, and what this test does about it — THE TEST-DESIGN
// LESSON, not a footnote. `_Static_assert` reports BOTH of its failure modes under the
// SAME code, `S_StaticAssertFailed` (S0029): a FALSE assertion and a NON-CONSTANT
// condition differ only in the message text. So a positive/negative `_Static_assert`
// pair produces the IDENTICAL code in both arms, and identical outcomes were read as
// proof that the fold worked when in fact NOTHING folded. This test therefore never
// discriminates on S0029 alone. Its three arms have three DIFFERENT outcomes:
//     positive  → ZERO diagnostics of any code;
//     false     → S_StaticAssertFailed whose message says "static assertion failed"
//                 and, asserted explicitly, does NOT say "not an integer constant
//                 expression";
//     truncation→ S_NonConstantArrayLength — a DIFFERENT diagnostic code entirely.
//
// RED-ON-DISABLE (run, then restored): delete the `case TypeKind::I128:` /
// `case TypeKind::U128:` rows from `resolveCastTarget` and this test fails with 4
// unexpected S_StaticAssertFailed — while `Int128WideConstantNeverTruncates...` below
// STAYS GREEN, because a fold that never happens also never truncates. Neither test
// alone is sufficient; that is why both exist.
TEST(SemanticAnalyzerC, Int128ConstantExpressionFolds) {
    // Every row is TRUE, so a correct build emits nothing at all. The `__int128` rows
    // are present because signedness is a separate resolver field (`intSigned`) from
    // width, and a row that set the width but not the sign would still pass the
    // unsigned rows.
    auto cu = buildShippedUnit("c", {
        "_Static_assert((__uint128_t)5 == 5, \"cast to unsigned __int128\");\n"
        "_Static_assert(((__uint128_t)5 + 1) == 6, \"arithmetic on the folded value\");\n"
        "_Static_assert((__int128)-1 < 0, \"a signed __int128 compares SIGNED\");\n"
        "_Static_assert(!((__uint128_t)-1 < 0), \"an unsigned one compares UNSIGNED\");\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "a 128-bit integer constant expression must fold — the `_BitInt(128)` twin "
           "of the first row already did, and the two must not disagree";
}

// The NEGATIVE control for the test above, and the reason it cannot be vacuous: a
// 128-bit assertion that is FALSE must still FAIL, and must fail as a false assertion
// rather than as a non-constant one. Without this row, "fold everything to true" would
// pass the positive test.
//
// The message check is load-bearing, NOT decoration: both failure modes carry
// S_StaticAssertFailed, so the code alone cannot tell "the fold ran and the answer was
// false" from "nothing folded". Asserting the message text is the only available
// discriminator at this tier.
TEST(SemanticAnalyzerC, Int128FalseConstantExpressionStillFailsAsAssertion) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert((__uint128_t)5 == 6, \"deliberately false\");\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const all = model.diagnostics().all();
    std::size_t saCount = 0;
    std::string text;
    for (auto const& d : all) {
        if (d.code != DiagnosticCode::S_StaticAssertFailed) continue;
        ++saCount;
        text = d.actual;
    }
    ASSERT_EQ(saCount, 1u) << "a false 128-bit assertion must fail exactly once";
    EXPECT_NE(text.find("static assertion failed"), std::string::npos)
        << "must report a FALSE assertion; got: " << text;
    EXPECT_EQ(text.find("is not an integer constant expression"), std::string::npos)
        << "the condition DID fold — reporting it as non-constant would mean the "
           "128-bit cast arm is dead again; got: " << text;
}

// ★★ THE DISQUALIFYING CASE, pinned: a 128-bit constant WIDER THAN 64 BITS must never
// reach a 64-bit ICE slot as a truncated value. It must FAIL LOUD instead.
//
// This is the property that makes activating the 128-bit cast arm safe rather than a
// hazard. The folded value rides a 128-bit `BitIntValue`, and `asInt64` nullopts for
// `width() > 64`, so `asInt64Bridge` — the array-dimension / static-assert / enumerator
// bridge — refuses it. MEASURED: `int a[(__uint128_t)1 << 100];` reports
// S_NonConstantArrayLength; it never becomes a truncated bound (mod 2^64 of 2^100 is
// ZERO, so a truncating fold would have produced a zero-length array, silently).
//
// The width-3 row is deliberate and is NOT redundant: it records that the refusal is
// keyed on the value's WIDTH, not on its magnitude, so a 128-bit constant that WOULD
// fit in 64 bits is refused too. That is exact parity with `_BitInt(128)`, which has
// behaved this way since C4b (MEASURED: `int a[(_BitInt(128))5];` →
// S_NonConstantArrayLength on this same build), and matching the shipped sibling is the
// point — the residual "clang folds `(__int128)2+1` as an array bound, we do not" gap
// belongs to `asInt64`'s width rule and is shared by BOTH 128-bit families, not
// introduced here.
//
// NOTE the code: S_NonConstantArrayLength, NOT the S_StaticAssertFailed the two tests
// above use. Different mechanism, different code — so no arm of this trio can be
// mistaken for another.
TEST(SemanticAnalyzerC, Int128WideConstantNeverTruncatesIntoA64BitSlot) {
    struct Row { char const* src; char const* what; };
    for (Row const r : {
             Row{"int a[(__uint128_t)1 << 100];\n",
                 "2^100 — mod 2^64 is ZERO, so a truncating fold means a silent "
                 "zero-length array"},
             Row{"int a[(__uint128_t)3];\n",
                 "a 128-bit value that WOULD fit in 64 bits is still refused — the "
                 "rule is the value's WIDTH, exactly as for _BitInt(128)"},
             Row{"int a[(__int128)3];\n",
                 "the signed spelling refuses identically"}}) {
        auto cu = buildShippedUnit("c", {std::string{r.src}});
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_NonConstantArrayLength), 1u)
            << "\n" << r.what;
    }
}

// The user-named combination: `_Atomic volatile int` must set BOTH bits in the ONE
// shared skin (cycle 1a's `qualified` merges {V}+{A}). Order-independent: the
// reverse spelling `volatile _Atomic int` resolves to the SAME interned type.
TEST(SemanticAnalyzerC, AtomicVolatileSetsBothQualifierBits) {
    auto cu = buildShippedUnit("c", {
        "_Atomic volatile int a;\n"
        "volatile _Atomic int b;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, PlainVolatileIsNotAtomicQualified) {
    auto cu = buildShippedUnit("c", {
        "volatile int v;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, AtomicPointeeVsPointerObject) {
    auto cu = buildShippedUnit("c", {
        "_Atomic int *p;\n"
        "int * _Atomic q;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, PointerCastsAccepted) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FloatToPointerCastRejected) {
    auto model = analyzeShipped("c", {
        "int main() { int* p; p = (int*)1.5; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidCast), 1u);
}

// D-CSUBSET-CAST-ARRAY-DECAY (FC3.5 sweep-c3): the cast OPERAND
// undergoes array-to-pointer decay BEFORE the legality check (C
// 6.3.2.1p3) — `(char*)"abc"` is Ptr↔Ptr after decay and `(long)"xy"`
// is decay + the Ptr→integer round-trip. Pre-sweep BOTH fired
// S_InvalidCast (the matrix saw Array(Char) raw).
TEST(SemanticAnalyzerC, CastOfStringLiteralDecaysAndIsAccepted) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, VoidDiscardCastAcceptsAllOperandTypes) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, CastFromVoidValueStaysRejected) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, TypedefCompoundLiteralResolvesAndTypes) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, CompoundLiteralUnknownTypeNameFiresUnknownType) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, CompoundLiteralValueIdentifierIsLoudError) {
    auto cu = buildShippedUnit("c", {
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
// these mirror the canonical cases through the REAL c config so a
// c.lang.json role-wiring regression (not just an engine bug) is
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
TEST(SemanticAnalyzerC, FnPtrDeclaratorTypedAsPtrFnSig) {
    auto model = analyzeShipped("c", { "int (*fp)(int);\n" });
    EXPECT_FALSE(model.hasErrors());
    expectPtrToIntIntFnSig(model.lattice().interner(),
                           typeOfSymbol(model, "fp"));
}

// `typedef int (*H)(int); H h;` — the typedef'd fn-ptr alias declares
// the SAME interned TypeId as the direct declarator form (structural
// canonicalization is the witness that the alias resolved through the
// identical inversion, not a lookalike).
TEST(SemanticAnalyzerC, TypedefFnPtrAliasDeclaresSameInternedType) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, StarBindsPerDeclaratorOnShippedGrammar) {
    auto model = analyzeShipped("c", { "int *p, q;\n" });
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
TEST(SemanticAnalyzerC, ArrayOfFnPtrDeclaratorTyped) {
    auto model = analyzeShipped("c", { "int (*arr[2])(int);\n" });
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
TEST(SemanticAnalyzerC, VoidParamListDeclaresZeroParams) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, NamedVoidParamFiresInvalidVoidParamPositioned) {
    auto cu = buildShippedUnit("c", {
        "int g(void x) { return 1; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidVoidParam), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_InvalidVoidParam) continue;
        // ⚠ SLICE THE BUFFER THE DIAGNOSTIC POINTS AT, not `trees()[0].source()`.
        // A preprocessed tree's `source()` is the SYNTH buffer, while a
        // diagnostic's span has been REMAPPED onto its ORIGIN buffer
        // (`CompilationUnit::remapPreprocessedPosition`) — so the two are in
        // different coordinate systems the moment anything precedes the main
        // source in the synth text: a spliced `#include`, a `--define`, or the
        // "<built-in>" prologue.
        // ★ This read was latent-wrong and passed only because all three were
        // empty on this path. It surfaced when D-PP-PREDEFINE-REDEFINITION-PARTITION
        // made the prologue non-empty on every format: the slice came back
        // "e __BI", six bytes of a prologue `#define` line, instead of "void x".
        // Nothing about the DIAGNOSTIC changed — only whether the wrong buffer
        // happened to coincide with the right one.
        std::string_view text;
        for (auto const& buf : cu->auxiliaryBuffers()) {
            if (buf != nullptr && buf->id() == d.buffer) {
                text = buf->slice(d.span);
                break;
            }
        }
        if (text.empty() && d.buffer == cu->trees()[0].source().id()) {
            text = cu->trees()[0].source().slice(d.span);
        }
        EXPECT_EQ(text, "void x")
            << "the diagnostic must span the offending param";
    }
}

// Unnamed params (C23: a lone type name in param position is ALWAYS a
// type) still contribute their types: `int h(int, char)` -> FnSig with
// exactly [I32, Char].
TEST(SemanticAnalyzerC, UnnamedParamsBuildTwoParamFnSig) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, TypedefNameStarDeclaresPointerLocal) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ValueStarValueStaysExpressionStatement) {
    auto model = analyzeShipped("c", {
        "int main() { int a = 2; int b = 3; a * b; return a; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    // main + a + b + the 3 FC12a-core builtin TYPES (__va_list_tag + va_list + the
    // __builtin_va_list alias, D-CSUBSET-BUILTIN-VA-LIST-TYPE-NAME) + the
    // 6 intrinsic builtins (c103 __umulh + c104 _InterlockedCompareExchange + c113
    // _ReadWriteBarrier + TF-C95 __sync_synchronize (D-CSUBSET-ATOMIC-FENCE) + c115
    // _exception_code + _exception_info) + the 6 FC17.9(b)
    // bit-count builtins (__builtin_{popcount,clz,ctz}{,ll},
    // D-CSUBSET-BITCOUNT-INTRINSICS) + the 56 FC17.9(b) <stdbit.h>
    // __builtin_stdc_<op>_<T> intrinsics (D-FULLC-STDBIT) + the 2 FC17.9(d) atomic
    // accessors (atomic_load_explicit + atomic_store_explicit, D-CSUBSET-ATOMIC) +
    // the 4 FC17.9(f) complex builtins (__builtin_complex/creal/cimag/conj,
    // D-CSUBSET-COMPLEX) + the 6 D-CSUBSET-INTRINSIC-BSWAP byte-swap builtins
    // (_byteswap_ushort/_byteswap_ulong/_byteswap_uint64 + __builtin_bswap16/32/64) +
    // the 2 FC17.5 predefined function-name symbols (__func__ + __FUNCTION__) — the
    // multiplication must mint NO symbol.
    EXPECT_EQ(model.symbols().size() - 1, 88u)
        << "main + a + b + __va_list_tag + va_list + __builtin_va_list + "
           "the 6 intrinsic builtins + "
           "the 6 __builtin bit-count intrinsics + the 56 __builtin_stdc_* "
           "<stdbit.h> intrinsics + atomic_load_explicit + atomic_store_explicit + "
           "the 4 __builtin_complex/creal/cimag/conj complex builtins + "
           "the 6 byte-swap builtins (_byteswap_ushort/_byteswap_ulong/"
           "_byteswap_uint64 + __builtin_bswap16/32/64) + "
           "__func__ + __FUNCTION__ — the multiplication mints none";
}

// UNKNOWN `u * v;` (no `u` anywhere, single file) — the oracle-candidate
// path: the follower-operator test rolls back (the `*` continues a value
// reading), the statement stays an EXPRESSION, and BOTH unknowns fire
// positioned S_UndeclaredIdentifier. Layout:
//   "int main() {\n  u * v;\n  return 0;\n}\n"
//    0-12 line 1; line 2 starts at 13; u at 15; v at 19.
TEST(SemanticAnalyzerC, UnknownStarUnknownStaysExpressionWithUndeclared) {
    auto cu = buildShippedUnit("c", {
        "int main() {\n  u * v;\n  return 0;\n}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, BareFnPtrCallTypesAndChecks) {
    // Clean: fp(3) against int(*)(int).
    auto clean = analyzeShipped("c", {
        "int helper(int v) { return v; }\n"
        "int main() { int (*fp)(int) = &helper; return fp(3); }\n",
    });
    EXPECT_FALSE(clean.hasErrors())
        << "a well-typed indirect call must be CLEAN (the c1 wall is "
           "retired)";

    // Arity: fp(1, 2) against int(*)(int) -> exactly 1 S_ArgCountMismatch.
    auto arity = analyzeShipped("c", {
        "int helper(int v) { return v; }\n"
        "int main() { int (*fp)(int) = &helper; return fp(1, 2); }\n",
    });
    EXPECT_EQ(countCode(arity.diagnostics(),
                        DiagnosticCode::S_ArgCountMismatch), 1u)
        << "indirect calls must get the SAME arity checking as direct";
    EXPECT_EQ(countCode(arity.diagnostics(),
                        DiagnosticCode::S_NotCallable), 0u);

    // Arg type: passing a pointer where the FnSig declares int.
    auto badArg = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, BareFunctionNameDecaysToPointerInEveryPosition) {
    // (a) bare ASSIGNMENT — the exact regression.
    auto assign = analyzeShipped("c", {
        "int add(int a, int b) { return a + b; }\n"
        "int main() { int (*fp)(int, int); fp = add; return fp(40, 2); }\n",
    });
    EXPECT_EQ(countCode(assign.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "a bare function name assigned to a matching function pointer "
           "(`fp = add`) must decay to its address, not fail S_TypeMismatch";

    // (b) bare INITIALIZER (no `&`).
    auto init = analyzeShipped("c", {
        "int add(int a, int b) { return a + b; }\n"
        "int main() { int (*fp)(int, int) = add; return fp(40, 2); }\n",
    });
    EXPECT_EQ(countCode(init.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "a bare function name in an initializer must decay";

    // (c) bare CALL-ARGUMENT (the callback position — `fn_fnptr_callback`).
    auto callback = analyzeShipped("c", {
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
    auto mismatch = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, CastFnPtrCalleeTypesAndChecks) {
    auto clean = analyzeShipped("c", {
        "int helper(int v) { return v; }\n"
        "typedef int (*H)(int);\n"
        "int main() { int (*fp)(int) = &helper; return ((H)fp)(3); }\n",
    });
    EXPECT_FALSE(clean.hasErrors());

    auto arity = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, CastNonCallableCalleeFiresNotCallable) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ParenWrappedDirectCalleeStaysClean) {
    auto model = analyzeShipped("c", {
        "int helper(int v) { return v + 2; }\n"
        "int main() { return (helper)(40); }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NotCallable), 0u);

    // FC4 c2: wrong arity through the paren-wrapped designator is now
    // CAUGHT (c1 deliberately admitted it silently) — and exactly ONCE.
    auto arity = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, DerefFnPtrCalleeTypesAndChecks) {
    auto clean = analyzeShipped("c", {
        "int helper(int v) { return v; }\n"
        "int main() { int (*fp)(int) = &helper; return (*fp)(3); }\n",
    });
    EXPECT_FALSE(clean.hasErrors())
        << "(*fp)(3) is a well-typed indirect call — must be clean";

    auto arity = analyzeShipped("c", {
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
    auto derefDesignator = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, VaArgWithBuiltinIntTypesClean) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, VaArgWithTypedefTypeResolves) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, VaArgWithValueInTypePositionFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, VaStartWithNonVaListFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FnPrototypeThenDefinitionMerges) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FnPrototypeIdempotentDeclarations) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FnDefinitionThenPrototypeMerges) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FnPrototypeIncompatibleRedeclarationFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FnPrototypeForwardCallResolvesSemantically) {
    auto model = analyzeShipped("c", {
        "int f(int);\n"
        "int g(void){return f(1);}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a forward call through a prototype is legal at the semantic tier";
    EXPECT_EQ(countSurvivingFns(model, "f"), 1u)
        << "the prototype is upgraded to a callable Function symbol";
}

// ── C34c (D-CSUBSET-FN-TYPEDEF-PROTOTYPE) — a `T x;` where T is a function TYPE
// (via a typedef) declares a function PROTOTYPE (C 6.7 / 6.9.1p2). This is
// SQLite test_thread.c's `static Tcl_ObjCmdProc sqlthread_proc;` shape. ──
//
// (g) A bare function-typedef declaration + its definition MERGE — exactly like a
// syntactic `int f(int);` proto. Zero diagnostics, one surviving Function symbol
// (the definition; the typedef proto absorbed). RED-ON-DISABLE: revert the Pass-1
// candidate flag + the Pass-1.5 upgrade → the bare `static Fn foo;` is minted an
// OBJECT → S_InvalidFunctionDeclarator (S0018) at the proto AND, because the
// object-category clashes with the Function definition, S_RedeclaredSymbol (S0002)
// at the definition → hasErrors() flips true and countSurvivingFns drops.
TEST(SemanticAnalyzerC, FnTypedefPrototypeThenDefinitionMerges) {
    auto model = analyzeShipped("c", {
        "typedef int Fn(int);\n"
        "static Fn foo;\n"
        "static int foo(int x){return x;}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a function-typedef prototype + its definition must merge (C 6.9.1p2)";
    EXPECT_EQ(countSurvivingFns(model, "foo"), 1u)
        << "exactly one surviving Function symbol for foo (the definition)";
}

// (h) The test_thread shape end-to-end: the bare function-typedef proto is
// forward-CALLED before its definition (as Tcl_CreateObjCommand takes
// `sqlthread_proc` above its body). Zero diagnostics; the call resolves to the
// upgraded Function symbol.
TEST(SemanticAnalyzerC, FnTypedefPrototypeForwardCallResolves) {
    auto model = analyzeShipped("c", {
        "typedef int Fn(int);\n"
        "static Fn foo;\n"
        "int use(void){return foo(41);}\n"
        "static int foo(int x){return x + 1;}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a forward call through a function-typedef prototype is legal";
    EXPECT_EQ(countSurvivingFns(model, "foo"), 1u);
}

// (i) FAIL-LOUD: a bare typedef-headed object whose type is NOT a function still
// collides with a same-name function definition. `MyInt foo;` (a real object) then
// `int foo(int){…}` is a genuine object-vs-function clash — the Pass-1 optimistic
// merge is a CANDIDATE only; the post-1.5 sweep sees `foo`'s type is not a FnSig
// and emits the DEFERRED, PRECISE S_RedeclaredSymbol. RED-ON-DISABLE: drop the
// post-1.5 non-FnSig re-check → the clash STILL fails loud, but via the generic
// type-compat check as S_IncompatibleRedeclaration instead — so the specific
// S_RedeclaredSymbol count drops to 0 and this assertion flips red. Exactly one
// S_RedeclaredSymbol.
TEST(SemanticAnalyzerC, FnTypedefObjectVsFunctionCollisionFailsLoud) {
    auto model = analyzeShipped("c", {
        "typedef int MyInt;\n"
        "MyInt foo;\n"
        "int foo(int x){return x;}\n",
    });
    EXPECT_TRUE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "a typedef-OBJECT redeclared as a function must fail loud (S0002), not "
           "be silently accepted as a prototype";
}

// (j) FAIL-LOUD (no over-broadening): a function-TYPED struct FIELD is not a
// prototype (a field can never be a function) — it still rejects with
// S_InvalidFunctionDeclarator. Pass 1 flags ONLY object-declaration rows as
// candidates, never a struct field, so the S0018 fail-loud is preserved.
TEST(SemanticAnalyzerC, FnTypedefFunctionTypedStructFieldRejected) {
    auto model = analyzeShipped("c", {
        "typedef int Fn(int);\n"
        "struct S { Fn f; };\n",
    });
    EXPECT_TRUE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 1u)
        << "a function-typed struct field is not a prototype and must fail loud";
}

// (k) A function-POINTER OBJECT (`Fn *fp;` — a decorated declarator) is NOT a
// prototype candidate: its declared type is Ptr<FnSig>, not FnSig. It stays a data
// object, so a same-name function is a genuine (immediate) collision. Pins that the
// candidate detection excludes decorated declarators (a star), never treating a
// function-pointer global as a function.
TEST(SemanticAnalyzerC, FnTypedefPointerObjectIsNotAPrototype) {
    auto model = analyzeShipped("c", {
        "typedef int Fn(int);\n"
        "Fn *fp;\n"
        "int fp(int x){return x;}\n",
    });
    EXPECT_TRUE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "a function-pointer object redeclared as a function is a collision";
}

// (l) FAIL-LOUD (F2 hardening): an illegal function-returning-function whose
// fn-suffix hides behind a redundant-paren group (`Fn (f)(int);` — C 6.7.6.3p1) is
// NOT a bare prototype. ✔MEASURED: gcc 13.3.0 refuses it ("'f' declared as function
// returning a function") and so does clang 18.1.3 ("function cannot return function
// type").
//
// ★★ WHAT REFUSES IT CHANGED IN P41, AND THE OLD ANSWER WAS AN ACCIDENT — see
// [[D-CSUBSET-PARENTHESIZED-FUNCTION-DEFINITION-DECLARATOR-REFUSED]]. This paragraph
// used to credit `declaratorIsUndecoratedName`'s upward walk, which merely kept `f`
// off the typedef-prototype path; the actual refusal then came from the FnSig-typed-
// Variable arm, whose message ("function prototype declarations are not supported
// here; use 'extern' …") names a construct this source does not contain, and which
// only fired because the SYNTACTIC shape walk could not see a function suffix through
// a redundant parenthesis. P41 taught that walk to see through parentheses — C 6.7.6
// permits them and `int (foo)(int x){…}` is legal C — so the accident is gone and the
// constraint is now checked ON PURPOSE, off the RESOLVED type, with a message that
// names it. That check also covers the INLINE spelling `int f(int)(int);`, which the
// accident never reached at all.
//
// RED-ON-DISABLE (restated to match): remove the C 6.7.6.3p1 result-type check in
// `resolveDeclTypes` (semantic_analyzer.cpp) → `f` is upgraded to a Function proto
// and SILENTLY ACCEPTED (no diagnostic, hasErrors() false).
TEST(SemanticAnalyzerC, FnTypedefParenGroupFnSuffixIsNotAPrototype) {
    auto model = analyzeShipped("c", {
        "typedef int Fn(int);\n"
        "Fn (f)(int);\n",
    });
    EXPECT_TRUE(model.hasErrors())
        << "an illegal function-returning-function (fn-suffix behind a group) must "
           "fail loud, never be silently accepted as a prototype";
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 1u);
}

// (m) A redundant grouping paren that adds NO suffix (`Fn (g);` ≡ `Fn g;`) is still
// an undecorated bare declarator, so it remains a valid function-typedef prototype
// and MERGES with its definition. Pins that the upward walk ADMITS pure grouping
// (rejecting only a group that CARRIES a decoration, as in (l)) — a walk that
// blanket-rejected any group would spuriously fail this legal proto.
TEST(SemanticAnalyzerC, FnTypedefRedundantParenGroupIsAPrototype) {
    auto model = analyzeShipped("c", {
        "typedef int Fn(int);\n"
        "Fn (g);\n"
        "int g(int x){return x + 1;}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a redundant-paren bare declarator is still a prototype (no suffix)";
    EXPECT_EQ(countSurvivingFns(model, "g"), 1u);
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
TEST(SemanticAnalyzerC, NoreturnProtoMergesIntoDefinition) {
    auto model = analyzeShipped("c", {
        "_Noreturn void die(int);\n"
        "void die(int x){ while(1){} }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a _Noreturn proto + a compatible definition must merge cleanly";
    EXPECT_EQ(countSurvivingFns(model, "die"), 1u);
    EXPECT_TRUE(survivingFnIsNoreturn(model, "die"))
        << "the _Noreturn on the proto must OR-merge into the surviving definition";
}

// TF-C79 (D-CSUBSET-INLINE-FUNCTION-SPECIFIER): the surviving Function symbol
// named `name` carries the C99 6.7.4p7 inline-definition reading. The
// `survivingFnIsNoreturn` mirror — the `!isAbsorbedProto` filter isolates the
// single callable record CST→HIR consults when it decides whether to emit a
// body at all.
[[nodiscard]] inline bool
survivingFnIsInline(SemanticModel const& model, std::string_view name) {
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        auto const& r = model.symbols()[i];
        if (r.name == name && r.kind == DeclarationKind::Function
            && !r.isAbsorbedProto) {
            return r.isInline;
        }
    }
    return false;
}

// TF-C79 (D-CSUBSET-INLINE-FUNCTION-SPECIFIER): the C99 6.7.4p7 truth table at
// the APPLIED-FACT tier — one case per DECLARATION SHAPE, each asserting the
// bit CST→HIR actually reads rather than "the file compiled".
//
// ★ EVERY EXPECTATION BELOW IS GROUND TRUTH FROM A REAL TOOLCHAIN, not from
// reading the standard. Each row was MEASURED with
// `/usr/bin/clang -std=c99 -O0 -c <case>.c` followed by `nm`: an inline
// definition shows `_p` UNDEFINED, an external definition shows `T _p`, and an
// internal one shows `t _p`. `isInline == true` is exactly the `U _p` column.
//
// ★★ ROWS (c) AND (d) ARE THE AND-MERGE PINS, and they are why this table
// exists as a table. 6.7.4p7 quantifies over ALL file-scope declarations —
// "if ALL of the file scope declarations ... include the inline function
// specifier without extern" — so the proto/def merge is an AND. Every other
// flag on SymbolRecord merges with OR, and an OR here would invert BOTH of
// these rows while leaving (a), (b) and (g) green: the two commonest real
// shapes would silently stop being emitted. Row (e) is the same quantifier
// reached through `extern` instead of through a missing `inline`, and row (f)
// pins the scanner's own without-extern clause.
TEST(SemanticAnalyzerC, InlineDefinitionFollowsC99Quantifier) {
    // (a) the lone inline definition — no other declaration to cancel it.
    {
        auto model = analyzeShipped("c",
                                    {"inline int p(int x){return x+1;}\n"});
        EXPECT_FALSE(model.hasErrors());
        EXPECT_TRUE(survivingFnIsInline(model, "p"))
            << "a lone `inline` definition IS an inline definition (clang: U _p)";
    }
    // (b) every declaration inline — the quantifier still holds.
    {
        auto model = analyzeShipped("c",
                                    {"inline int p(int x);\n"
                                     "inline int p(int x){return x+1;}\n"});
        EXPECT_FALSE(model.hasErrors());
        EXPECT_TRUE(survivingFnIsInline(model, "p"))
            << "all-inline declarations keep the inline definition (clang: U _p)";
    }
    // (c) ★ inline PROTOTYPE + plain definition — the quantifier fails.
    {
        auto model = analyzeShipped("c",
                                    {"inline int p(int x);\n"
                                     "int p(int x){return x+1;}\n"});
        EXPECT_FALSE(model.hasErrors());
        EXPECT_FALSE(survivingFnIsInline(model, "p"))
            << "one plain declaration restores the external definition "
               "(clang: T _p) — an OR-merge would wrongly report true here";
    }
    // (d) ★ plain prototype + inline definition — the same, other order.
    {
        auto model = analyzeShipped("c",
                                    {"int p(int x);\n"
                                     "inline int p(int x){return x+1;}\n"});
        EXPECT_FALSE(model.hasErrors());
        EXPECT_FALSE(survivingFnIsInline(model, "p"))
            << "declaration ORDER must not change the quantifier (clang: T _p)";
    }
    // (e) ★ an `extern` declaration cancels it — the silent-halfway-state pin.
    {
        auto model = analyzeShipped("c",
                                    {"extern int p(int x);\n"
                                     "inline int p(int x){return x+1;}\n"});
        EXPECT_FALSE(model.hasErrors());
        EXPECT_FALSE(survivingFnIsInline(model, "p"))
            << "a co-present extern declaration promotes the definition "
               "(clang: T _p) — losing this is the one SILENT failure mode";
    }
    // (f) ★ `extern inline` on the definition itself — 6.7.4p7's exemption,
    // caught by the scanner's own without-extern clause rather than by a merge.
    {
        auto model = analyzeShipped("c",
                                    {"extern inline int p(int x){return x+1;}\n"});
        EXPECT_FALSE(model.hasErrors());
        EXPECT_FALSE(survivingFnIsInline(model, "p"))
            << "`extern inline` DOES provide the external definition "
               "(clang: T _p)";
    }
    // (g) `static inline` — the flag is set; internal linkage is what keeps the
    // body emitted (6.7.4p6), and that gate lives at the HIR binding check, not
    // here. Pinning true rather than false keeps the two concerns separable.
    {
        auto model = analyzeShipped("c",
                                    {"static inline int p(int x){return x+1;}\n"});
        EXPECT_FALSE(model.hasErrors());
        EXPECT_TRUE(survivingFnIsInline(model, "p"))
            << "static inline records the specifier; the Local binding is what "
               "keeps it emitted (clang: t _p)";
    }
}

// TF-C79 (D-CSUBSET-INLINE-FUNCTION-SPECIFIER): `inline`, `__inline` and
// `__inline__` are ONE token kind and therefore one meaning.
//
// The synonym claim is not a style assertion — it is the reason the keyword
// table maps three words to a single kind instead of forking a GNU dialect.
// MEASURED: `/usr/bin/clang -std=c99 -O0 -c` gives `__inline int p(int){…}` the
// same `U _p` symbol state as the C99 spelling. RED-ON-DISABLE: give `__inline`
// its own kind and it stops reaching `semantics.inline.keywordToken`, so these
// two flip to false while the `inline` row above stays green — exactly the
// silent split the single kind exists to prevent.
TEST(SemanticAnalyzerC, InlineGnuSpellingsAreSynonyms) {
    for (std::string_view spelling : {"__inline", "__inline__"}) {
        auto model = analyzeShipped(
            "c", {std::string(spelling) + " int p(int x){return x+1;}\n"});
        EXPECT_FALSE(model.hasErrors()) << spelling;
        EXPECT_TRUE(survivingFnIsInline(model, "p"))
            << spelling << " must mean exactly what `inline` means";
    }
}

// TF-C79 (D-CSUBSET-INLINE-FUNCTION-SPECIFIER): C99 6.7.4p1 confines the
// specifier to "the declaration of a function", so `inline` on an OBJECT is a
// constraint violation and must be LOUD.
//
// ★ WHY LOUD RATHER THAN INERT, unlike a stray `_Noreturn`. On a function this
// specifier decides whether the definition is emitted at all; silently ignoring
// it on a non-function would be a specifier the compiler parsed and honored
// nowhere — D-TEST-IGNORE-LIST-IS-A-LICENSE-TO-DROP at the declaration tier.
// The LINKAGE tier cannot raise it (it sees the keyword but not the declared
// type), which is why the keyword is ignored-by-kind there and reported here.
TEST(SemanticAnalyzerC, InlineOnNonFunctionIsLoud) {
    auto model = analyzeShipped("c", {"inline int gv = 5;\n"});
    EXPECT_TRUE(model.hasErrors())
        << "`inline` on an object must not be silently ignored";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InlineNonFunction), 1u);
}

// ── TF-C81 (D-CSUBSET-ALWAYSINLINE): the CONTRADICTION gate ───────
//
// `always_inline` and `noinline` on one function are exact opposites: one
// forbids splicing the callee, the other exists solely to force splicing past
// the cost model. Exactly one can take effect and which one is invisible at the
// source, so DSS FAILS LOUD instead of picking.
//
// ★ THIS IS A DELIBERATE DIVERGENCE FROM CLANG, AND IT IS MEASURED, NOT
// ASSUMED. Apple clang 21.0.0 was probed on all four shapes at
// `-fsyntax-only -Wall -Wextra` AND at `-O2 -Weverything`: it emits NO
// diagnostic whatsoever and SILENTLY resolves the conflict to `noinline` (the
// emitted LLVM function carries `noinline`, never `alwaysinline`, in BOTH source
// orders). That silent resolution is exactly the last-writer-wins outcome this
// project refuses. The MIR tier keeps clang's answer as a conservative backstop
// (inlining.cpp checks the noinline refusal before the threshold bypass), but a
// source program that states both never gets that far.
//
// Diagnostic SETS, never bare counts: each case asserts the conflict code fires
// exactly once AND that no unrelated error rides along.
TEST(SemanticAnalyzerC, AlwaysInlineWithNoInlineOnOneDeclarationIsLoud) {
    auto model = analyzeShipped(
        "c",
        {"static __attribute__((always_inline)) __attribute__((noinline)) "
         "int f(int x){return x+1;}\n"
         "int main(void){return f(41);}\n"});
    EXPECT_TRUE(model.hasErrors())
        << "two contradictory inline directives must not be silently resolved";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConflictingInlineAttributes), 1u);
    EXPECT_EQ(model.diagnostics().all().size(), 1u)
        << "exactly ONE diagnostic — the conflict, and nothing else";
}

// The same contradiction SPLIT across a prototype and its definition. Each
// declaration alone is consistent; only the post-Pass-1.5 `mergedFnDecls` sweep
// can see it.
//
// ★ RED-ON-DISABLE, AND THIS ONE ACTUALLY FIRED DURING THE CYCLE: the gate was
// first written AFTER the `noInline` OR-merge, which had already written the
// unioned flag into BOTH records — so the survivor looked like it had spelled
// both attributes itself, the "already reported" suppression triggered, and the
// split conflict was silently accepted (MEASURED: exit 0, no diagnostic). Moving
// the gate ABOVE both OR-merges, where the two records are still pristine, is
// what makes it fire. Reordering it back below them turns this test red.
TEST(SemanticAnalyzerC, AlwaysInlineWithNoInlineAcrossDeclarationsIsLoud) {
    for (auto const* src : {
             // noinline on the prototype, always_inline on the definition
             "static __attribute__((noinline)) int f(int x);\n"
             "static __attribute__((always_inline)) int f(int x){return x+1;}\n"
             "int main(void){return f(41);}\n",
             // …and the opposite order, which must be equally loud
             "static __attribute__((always_inline)) int f(int x);\n"
             "static __attribute__((noinline)) int f(int x){return x+1;}\n"
             "int main(void){return f(41);}\n"}) {
        SCOPED_TRACE(src);
        auto model = analyzeShipped("c", {src});
        EXPECT_TRUE(model.hasErrors());
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_ConflictingInlineAttributes), 1u)
            << "the split contradiction must fire EXACTLY once, in either "
               "declaration order";
        EXPECT_EQ(model.diagnostics().all().size(), 1u);
    }
}

// One mistake, one diagnostic: a declaration carrying BOTH attributes that ALSO
// has a prototype must not be reported twice (once at the declarator, once at
// the merge). This is what the `already` suppression in the merge sweep buys.
TEST(SemanticAnalyzerC, ConflictingInlineAttributesReportedExactlyOnce) {
    auto model = analyzeShipped(
        "c",
        {"static int f(int x);\n"
         "static __attribute__((always_inline)) __attribute__((noinline)) "
         "int f(int x){return x+1;}\n"
         "int main(void){return f(41);}\n"});
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConflictingInlineAttributes), 1u)
        << "the declarator check and the merge check must not BOTH fire for a "
           "single mistake";
    EXPECT_EQ(model.diagnostics().all().size(), 1u);
}

// The NEGATIVE control for the whole conflict suite: each attribute ALONE, and
// both present on DIFFERENT functions, must be completely clean. Without this a
// gate that fired on the mere presence of either attribute would pass every
// test above.
TEST(SemanticAnalyzerC, EitherInlineAttributeAloneIsClean) {
    for (auto const* src : {
             "static __attribute__((always_inline)) int f(int x){return x+1;}\n"
             "int main(void){return f(41);}\n",
             "static __attribute__((noinline)) int f(int x){return x+1;}\n"
             "int main(void){return f(41);}\n",
             // both attributes present, but on DIFFERENT functions
             "static __attribute__((always_inline)) int a(int x){return x+1;}\n"
             "static __attribute__((noinline)) int b(int x){return x+2;}\n"
             "int main(void){return a(1)+b(2);}\n"}) {
        SCOPED_TRACE(src);
        auto model = analyzeShipped("c", {src});
        EXPECT_FALSE(model.hasErrors())
            << "a single inline directive — or two on DIFFERENT functions — "
               "is not a conflict";
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_ConflictingInlineAttributes), 0u);
    }
}

// ══ TF-C92 (D-CSUBSET-NO-SANITIZE-THREAD): the SYMBOL-TIER facts ═══════════════
//
// The downstream hops (HirNoSanitizeThreadMap → MirFunc → `.dssir` text) are pinned
// in tests/mir; these three pin the SOURCE-TIER contract at the record itself, where
// a failure is localized to the fold/apply/merge rather than to lowering.
//
// ★ THE OR-MERGE IS ASSERTED ON **BOTH** RECORDS, and that is not belt-and-braces.
// A call resolves to the DEFINITION (the survivor), so the load-bearing direction is
// INTO the survivor — but the merge writes both, and `mergedFnDecls` order is not a
// property a test should depend on. Asserting both makes the pin order-independent.
//
// RED-ON-DISABLE: delete the `isNoSanitizeThread` OR-merge block from the
// `mergedFnDecls` sweep in semantic_analyzer.cpp and the prototype-only case below
// leaves the DEFINITION's record false — which is exactly the shape that would ship
// a silently unrecorded exclusion for the ordinary header/impl split.
TEST(SemanticAnalyzerC, NoSanitizeThreadOrMergesAcrossPrototypeAndDefinition) {
    for (auto const* src : {
             // spelled on the PROTOTYPE only — the glibc-style header/impl split
             "static int f(int x) __attribute__((no_sanitize_thread));\n"
             "static int f(int x){return x+1;}\n"
             "int main(void){return f(41);}\n",
             // …and on the DEFINITION only, which must merge the other way
             "static int f(int x);\n"
             "static __attribute__((no_sanitize_thread)) int f(int x){return x+1;}\n"
             "int main(void){return f(41);}\n"}) {
        SCOPED_TRACE(src);
        auto model = analyzeShipped("c", {src});
        EXPECT_FALSE(model.hasErrors())
            << "the attribute alone is not an error: "
            << (model.diagnostics().all().empty()
                    ? "" : model.diagnostics().all()[0].actual);
        int marked = 0;
        int named  = 0;
        for (std::size_t i = 1; i < model.symbols().size(); ++i) {
            if (model.symbols()[i].name != "f") continue;
            ++named;
            if (model.symbols()[i].isNoSanitizeThread) ++marked;
        }
        ASSERT_GT(named, 0) << "the fixture must declare `f`";
        EXPECT_EQ(marked, named)
            << "EVERY record for `f` must carry the exclusion after the "
               "proto/definition OR-merge — the definition is what HIR→MIR stamps "
               "from, so a merge that only marks the spelling side loses the fact";
    }
}

// ★★★ TF-C92 → **FLIPPED BY TF-C93**
// (D-CSUBSET-ATTRIBUTE-IGNORED-FOR-DECL-KIND-SILENT). This test was written to PIN
// THE SILENCE while naming it wrong, with in-code instructions to flip rather than
// delete it. That is what happened: the fixture is byte-identical, the
// records-nothing loop is VERBATIM, and only the diagnostic expectation moved from
// "nothing at all" to "exactly one `S_AttributeIgnoredForDeclarationKind` Warning".
//
// ★★ THE FLIP HAD TO BE ASSERTED POSITIVELY, AND THAT IS THE POINT OF THE TEST —
// MEASURED. The new diagnostic is a WARNING, so `EXPECT_FALSE(model.hasErrors())`
// still passes with it present: this test STAYED GREEN through the entire engine
// change. A "flip" that only relaxed the old assertion would have shipped looking
// verified while proving nothing. Hence the exact-code assertion below, and hence
// the severity assertion beside it: a count-only check would also pass if
// `S_UnknownAttribute` or `S_InlineNonFunction` fired instead, and a code-only check
// would pass if a later cycle quietly promoted the code to an Error.
//
// ★ THE `EXPECT_FALSE(isNoSanitizeThread)` LOOP IS KEPT VERBATIM ON PURPOSE. The
// gate REPORTS; the `isFnSig &&` guard in the apply still REFUSES. A diagnostic that
// ALSO recorded the flag would be a WORSE bug than the silence — a data-object
// symbol carrying a codegen directive no consumer can honor — so both halves are
// asserted together, in one test, and neither can be satisfied by breaking the
// other.
//
// WHAT BREAKS THIS: deleting `appliesTo` from the `no_sanitize_thread` effects row
// (the LOADER refuses the config, so the whole suite goes red — the coupling is
// mechanical); deleting the decl-kind gate loop in semantic_analyzer.cpp (the
// diagnostic count drops to 0 while the loop below stays green); dropping the
// `isFnSig &&` guard from the apply (the loop below goes red while the diagnostic
// stays).
TEST(SemanticAnalyzerC, NoSanitizeThreadOnDataObjectWarnsAndRecordsNothing) {
    auto model = analyzeShipped(
        "c",
        {"__attribute__((no_sanitize_thread)) int gv = 7;\n"
         "int main(void){return gv;}\n"});
    EXPECT_FALSE(model.hasErrors())
        << "the decl-kind gate is a WARNING, not an error — clang treats the "
           "sibling axes as -Wignored-attributes warnings and "
           "--warnings-as-errors is the strict posture: "
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AttributeIgnoredForDeclarationKind), 1u)
        << "exactly ONE ignored-for-kind diagnostic, asserted BY CODE — a bare "
           "count would also pass if S_UnknownAttribute or S_InlineNonFunction "
           "fired instead, and `hasErrors()` cannot see a Warning at all";
    std::size_t warnings = 0;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_AttributeIgnoredForDeclarationKind)
            continue;
        if (d.severity == DiagnosticSeverity::Warning) ++warnings;
        EXPECT_NE(d.actual.find("no_sanitize_thread"), std::string::npos)
            << "the message must NAME the attribute the author has to move: "
            << d.actual;
        EXPECT_NE(d.actual.find("variable"), std::string::npos)
            << "…and the KIND it was found on, so the author can see why: "
            << d.actual;
    }
    EXPECT_EQ(warnings, 1u)
        << "the severity is part of the contract, not incidental — see the "
           "measured grounds on S_AttributeIgnoredForDeclarationKind";
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        EXPECT_FALSE(model.symbols()[i].isNoSanitizeThread)
            << "no symbol may carry the flag when the only annotated declaration "
               "is a data object — symbol '" << model.symbols()[i].name << "'";
    }
}

// ── ★★★ TF-C93: THE DECL-KIND MATRIX — ALL FOUR AXES × EVERY ENTITY KIND ──────
//
// (D-CSUBSET-ATTRIBUTE-IGNORED-FOR-DECL-KIND-SILENT.) The registry row called this
// a SYSTEMIC class and asked for ONE shared gate rather than a per-attribute fix, so
// the evidence has to be a MATRIX rather than one probe per cycle: a table-driven
// sweep is the only shape in which "we forgot an axis" is visible.
//
// ★ THE SQLITE CORPUS PROVES NOTHING HERE — MEASURED, ZERO corpus impact (the
// attribute appears in sqlite only in the FUNCTION position, which is exactly the
// position that must stay silent). These tests are the sole evidence.
//
// ★★ THE CLASS MEMBERSHIP IS **FOUR** AXES ACROSS **FIVE** CONFIG ROWS, AND NOT THE
// FOUR THE REGISTRY ROW FIRST NAMED. Among the attribute-EFFECT verbs, exactly
// `noInline`, `alwaysInline` and `noSanitizeThread` discard on `isFnSig &&` (that
// grep also finds `isFnSig` gates for `noreturn`, `inline` and `isNoOptimize`, which
// are SPECIFIER scans, not rows of this table — membership is decided per axis by
// whether the row's own sink discards on kind, never by counting grep hits).
// `aligned` is NOT a member — no such gate, its sink discriminates by kind itself,
// and it is the NEGATIVE CONTROL here. The `warnOnDiscard` axis IS a member and was
// missed: MEASURED at HEAD 199fe7d, `int gw1 __attribute__((warn_unused_result)) = 1;`
// compiled clean with ZERO diagnostics where clang warns, because the apply has no
// kind gate at all and the flag landed on an object `checkCall` can never consult.
//
// ★ THAT AXIS IS **TWO ROWS**, WHICH IS WHY THE ROW COUNT EXCEEDS THE AXIS COUNT.
// `nodiscard` and `warn_unused_result` share the verb but NOT the applicable-kind
// set — the standard spelling is function-only per C23 6.7.13.3, the GNU spelling
// also applies to typedefs per clang's own applicability text. Collapsing them into
// one row is what produced this cycle's false positive; see `kDeclKindAxes`.
namespace {
// ★★ ONE ROW OF THE MATRIX — AND IT DECLARES THE **KIND SET**, NOT A PER-TEST
// VERDICT. Each position test below DERIVES its expectation from this set: the
// shared gate warns exactly when the position's `DeclarationKind` is ABSENT from the
// row's `appliesTo`. One per-axis fact therefore drives all three position tests,
// every column is READ, and a config change that is not mirrored here goes RED.
//
// ⚠ THIS SHAPE IS A CORRECTION, AND THE SHAPE IT REPLACES BENT THE CONFIG — recorded
// because a test that manufactures the fact it checks is the failure mode this whole
// file exists to prevent. The first draft carried a single `bool appliesToFn`, set on
// all nine rows and NEVER READ, while the typedef pin below (then named
// `…MatrixTypedefWarns`, renamed `…MatrixTypedefWarnsPerAxis` because the old name
// asserted the fiction) demanded a warning from ALL NINE axes — i.e. UNIFORMITY
// instead of truth per axis. `appliesTo` for `warn_unused_result` was then narrowed
// to `["function"]` to satisfy it, and DSS began emitting `warning[S005F]` on
// `typedef __attribute__((warn_unused_result)) int T;` — MEASURED clang-clean. A
// matrix may assert uniformity ONLY where the axes are genuinely uniform; everywhere
// else the expectation has to travel with the row.
struct DeclKindAxis {
    char const* attr;          // the spelling written into the fixture
    bool        appliesToVar;  // is `variable` in this row's `appliesTo`?
    bool        appliesToFn;   // …`function`?
    bool        appliesToType; // …`type`?
};

// The four axes, in BOTH the bare and the dunder spelling. The effects table is
// dunder-normalized (ONE row covers both), and that is asserted rather than assumed
// — a future revision that keyed on raw text would silently lose every `__x__` form.
//
// ★ `warn_unused_result` IS THE ONE AXIS THAT DIFFERS, AND IT DIFFERS FROM ITS OWN
// C23 SIBLING. clang's applicability text ENUMERATES typedefs among the valid
// positions for the GNU spelling (MEASURED: zero diagnostics on all three typedef
// forms), while C23 6.7.13.3 confines `nodiscard` to a function / struct / union /
// enum declaration and clang refuses the object form for both. Two names, ONE
// effect, TWO applicability sets — which is why the shipped config gives them
// SEPARATE rows and why this column has to exist.
constexpr DeclKindAxis kDeclKindAxes[] = {
    //  spelling                    var    fn    type
    {"noinline",                   false, true, false},
    {"__noinline__",               false, true, false},
    {"always_inline",              false, true, false},
    {"__always_inline__",          false, true, false},
    {"no_sanitize_thread",         false, true, false},
    {"__no_sanitize_thread__",     false, true, false},
    {"warn_unused_result",         false, true, true },
    {"__warn_unused_result__",     false, true, true },
    {"nodiscard",                  false, true, false},
};

// Exactly-one-warning-of-this-code, with the severity checked. Returns the count so
// the caller can `EXPECT_EQ` — no `ASSERT_*` in a non-void helper (CI hazard).
[[nodiscard]] std::size_t ignoredForKindWarnings(SemanticModel const& m) {
    std::size_t n = 0;
    for (auto const& d : m.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_AttributeIgnoredForDeclarationKind
            && d.severity == DiagnosticSeverity::Warning) {
            ++n;
        }
    }
    return n;
}
} // namespace

// AXIS × FUNCTION — every one of the four must stay SILENT where the attribute
// genuinely applies. This is the anti-over-broad half, and it is what a gate written
// as "warn whenever the flag is dropped" would fail: the function position is the one
// sqlite actually uses (`wal.c`'s `walIndexWriteHdr` / `walIndexTryHdr`).
TEST(SemanticAnalyzerC, AttributeDeclKindMatrixFunctionPositionStaysSilent) {
    for (auto const& ax : kDeclKindAxes) {
        SCOPED_TRACE(ax.attr);
        // Definition AND prototype: the two function shapes reach the gate by
        // different routes (`isFunctionForm` vs `isFnSig`), and the extracted
        // effective-kind predicate has to answer `Function` for both.
        for (auto const* shape : {"static __attribute__(({})) int f(int x)"
                                  "{{return x+1;}}\nint main(void){{return f(1);}}\n",
                                  "__attribute__(({})) int f(int);\n"
                                  "int f(int x){{return x+1;}}\n"
                                  "int main(void){{return f(1);}}\n"}) {
            auto const src = std::vformat(
                shape, std::make_format_args(ax.attr));
            SCOPED_TRACE(src);
            auto m = analyzeShipped("c", {src});
            EXPECT_FALSE(m.hasErrors())
                << (m.diagnostics().all().empty()
                        ? "" : m.diagnostics().all()[0].actual);
            EXPECT_EQ(ignoredForKindWarnings(m),
                      ax.appliesToFn ? 0u : 1u)
                << "the FUNCTION position is where these attributes BELONG — a "
                   "gate that fires here refuses valid C (and, for the three "
                   "FnSig-gated verbs, the exact shape sqlite writes). Derived "
                   "from the row's declared kind set, so this stays honest if a "
                   "future axis is NOT function-applicable";
        }
    }
}

// AXIS × DATA OBJECT — the defect. Every axis must WARN, exactly once, and the flag
// must still NOT be recorded (report-and-refuse, never report-and-accept).
TEST(SemanticAnalyzerC, AttributeDeclKindMatrixDataObjectWarns) {
    for (auto const& ax : kDeclKindAxes) {
        SCOPED_TRACE(ax.attr);
        auto const src = std::vformat(
            "__attribute__(({})) int gv = 7;\nint main(void){{return gv;}}\n",
            std::make_format_args(ax.attr));
        auto m = analyzeShipped("c", {src});
        EXPECT_FALSE(m.hasErrors())
            << "a WARNING, so no program that compiled before stops compiling: "
            << (m.diagnostics().all().empty()
                    ? "" : m.diagnostics().all()[0].actual);
        EXPECT_EQ(ignoredForKindWarnings(m),
                  ax.appliesToVar ? 0u : 1u)
            << "MEASURED silent at HEAD 199fe7d — exactly one warning now. NO axis "
               "declares `variable`, so every row is expected loud here; the "
               "expectation is still DERIVED so that adding an object-applicable "
               "axis cannot silently turn this into a false positive";
        for (std::size_t i = 1; i < m.symbols().size(); ++i) {
            EXPECT_FALSE(m.symbols()[i].isNoInline)
                << "the report must not also RECORD the flag: " << ax.attr;
            EXPECT_FALSE(m.symbols()[i].isAlwaysInline) << ax.attr;
            EXPECT_FALSE(m.symbols()[i].isNoSanitizeThread) << ax.attr;
        }
    }
}

// AXIS × TYPEDEF — the position `linkageFrom` never reached (a `typedefDecl`
// declares no `linkageSpecifiers`, so the loud linkage gate early-returns), which is
// why it was silent for every axis and is asserted separately from the object case.
//
// ★★ THIS IS THE TEST THAT USED TO ASSERT A FICTION, AND THE SPLIT IS WHAT IT
// MEASURES NOW. It previously demanded a warning from ALL NINE axes; that uniformity
// is FALSE, and satisfying it is what narrowed `warn_unused_result`'s `appliesTo`
// until DSS diagnosed clang-clean C. The expectation is now DERIVED per axis:
//   • the six FnSig-gated spellings + `nodiscard`  → LOUD (typedef is out of bounds)
//   • `warn_unused_result` / `__warn_unused_result__` → SILENT, because clang's own
//     applicability list names typedefs and MEASURED emits nothing there.
// The silent arm is the load-bearing one: re-narrow that row and this test goes RED
// rather than quietly re-blessing the false positive.
TEST(SemanticAnalyzerC, AttributeDeclKindMatrixTypedefWarnsPerAxis) {
    for (auto const& ax : kDeclKindAxes) {
        SCOPED_TRACE(ax.attr);
        auto const src = std::vformat(
            "typedef __attribute__(({})) int T;\n"
            "int main(void){{T x = 0; return x;}}\n",
            std::make_format_args(ax.attr));
        auto m = analyzeShipped("c", {src});
        EXPECT_FALSE(m.hasErrors())
            << (m.diagnostics().all().empty()
                    ? "" : m.diagnostics().all()[0].actual);
        std::size_t const want = ax.appliesToType ? 0u : 1u;
        EXPECT_EQ(ignoredForKindWarnings(m), want)
            << (ax.appliesToType
                    ? "this spelling IS typedef-applicable (clang enumerates "
                      "typedefs and MEASURED emits nothing) — a warning here is "
                      "the FALSE POSITIVE this row's split exists to remove"
                    : "the typedef position must warn for this spelling — the gate "
                      "reads the effective DeclarationKind (Type here), not just "
                      "'is it a FnSig'");
        if (want == 0u) continue;
        for (auto const& d : m.diagnostics().all()) {
            if (d.code != DiagnosticCode::S_AttributeIgnoredForDeclarationKind)
                continue;
            EXPECT_NE(d.actual.find("type"), std::string::npos)
                << "and it must SAY 'type', so the author is not left guessing "
                   "which entity the compiler thinks it saw: " << d.actual;
        }
    }
}

// ★★ THE TYPEDEF SILENCE IS **NOT** VACUOUS — the two spellings must diverge on the
// SAME declaration shape, or the arm above could pass because the gate stopped
// working for typedefs altogether. This pins the DIVERGENCE itself: one row's name
// silent, the other's loud, same position, same fixture shape. It is also the
// red-on-disable witness for the row split — merge the two rows back under either
// `appliesTo` and exactly one of these two expectations fails.
TEST(SemanticAnalyzerC, AttributeDeclKindTypedefSplitsTheTwoDiscardSpellings) {
    struct Case { char const* attr; std::size_t want; char const* why; };
    for (Case const c : {
             Case{"warn_unused_result", 0u,
                  "the GNU spelling applies to a typedef (clang: zero diagnostics "
                  "on all three typedef forms, MEASURED)"},
             Case{"nodiscard", 1u,
                  "C23 6.7.13.3 confines the standard spelling to a function / "
                  "struct / union / enum declaration, so a typedef is out of "
                  "bounds and must stay loud"}}) {
        SCOPED_TRACE(c.attr);
        auto const src = std::vformat(
            "typedef __attribute__(({})) int T;\n"
            "int main(void){{T x = 0; return x;}}\n",
            std::make_format_args(c.attr));
        auto m = analyzeShipped("c", {src});
        EXPECT_FALSE(m.hasErrors())
            << (m.diagnostics().all().empty()
                    ? "" : m.diagnostics().all()[0].actual);
        EXPECT_EQ(ignoredForKindWarnings(m), c.want) << c.why;
    }
    // …and the OBJECT position stays loud for BOTH, so the split narrowed exactly
    // one position and did not quietly disarm the axis it was meant to keep.
    for (auto const* attr : {"warn_unused_result", "nodiscard"}) {
        SCOPED_TRACE(attr);
        auto const src = std::vformat(
            "int gw __attribute__(({})) = 1;\nint main(void){{return gw;}}\n",
            std::make_format_args(attr));
        auto m = analyzeShipped("c", {src});
        EXPECT_EQ(ignoredForKindWarnings(m), 1u)
            << "clang warns -Wignored-attributes on the object form for BOTH "
               "spellings (MEASURED) — widening either row to `variable` would "
               "silence a real misuse";
    }
}

// ★★ THE NEGATIVE CONTROL — `aligned(16)`. It is NOT a member of the class and the
// shared gate must stay SILENT for it in every position, because its own sink is the
// SOLE authority on that axis and already discriminates by kind:
//   * FUNCTION  → already LOUD, `error[S002F]` (S_AlignasInvalidContext, and that
//                 code is UNSUPPRESSABLE). If `appliesTo` omitted "function" the one
//                 mistake would DOUBLE-REPORT: this cycle's warning PLUS that error.
//   * OBJECT    → SILENT and HONORED (`explicitAlignment` is set) — correct C, and
//                 the case that proves the gate is not a blanket "attribute on an
//                 object" warning.
//   * TYPEDEF   → a GRADED judgement, unchanged by this cycle.
// Its `appliesTo` therefore lists every kind its sink judges, which is a deliberate
// statement about the SINK, not about the attribute's C semantics.
TEST(SemanticAnalyzerC, AttributeDeclKindMatrixAlignedIsTheNegativeControl) {
    {   // OBJECT: silent, honored.
        auto m = analyzeShipped(
            "c",
            {"__attribute__((aligned(16))) int ga = 7;\n"
             "int main(void){return ga;}\n"});
        EXPECT_FALSE(m.hasErrors())
            << (m.diagnostics().all().empty()
                    ? "" : m.diagnostics().all()[0].actual);
        EXPECT_EQ(ignoredForKindWarnings(m), 0u)
            << "`aligned` on an object is CORRECT C and is honored — a warning "
               "here means the gate widened into 'any attribute on an object'";
        bool honored = false;
        for (std::size_t i = 1; i < m.symbols().size(); ++i) {
            if (m.symbols()[i].name == "ga"
                && m.symbols()[i].explicitAlignment.has_value()
                && *m.symbols()[i].explicitAlignment == 16u) {
                honored = true;
            }
        }
        EXPECT_TRUE(honored)
            << "and the value must still REACH the symbol — the negative control "
               "is worthless if the attribute stopped working";
    }
    {   // FUNCTION: the shipped unsuppressable error, and ONLY it.
        auto m = analyzeShipped(
            "c",
            {"__attribute__((aligned(16))) int fa(void);\n"
             "int main(void){return fa();}\n"});
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_AlignasInvalidContext), 1u)
            << "the shipped S002F refusal is the sole authority on this axis";
        EXPECT_EQ(ignoredForKindWarnings(m), 0u)
            << "and it must not be JOINED by the shared warning — one mistake, "
               "one diagnostic. This is what `appliesTo` listing 'function' buys";
    }
    {   // EXTERN position: MEASURED reachable, same single-diagnostic property.
        auto m = analyzeShipped(
            "c",
            {"extern int fe1(void) __attribute__((aligned(16)));\n"
             "int main(void){return 0;}\n"});
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_AlignasInvalidContext), 1u)
            << "the externDecl row is REACHABLE for this axis (MEASURED) — it is "
               "not a position the gate may quietly skip";
        EXPECT_EQ(ignoredForKindWarnings(m), 0u);
    }
}

// ★★ A FUNCTION-POINTER OBJECT — `dk == Variable` while `isFnSig == false`, because
// the declared type is `Ptr<FnSig>` and not `FnSig`. This is the exact distinction
// D-CSUBSET-NORETURN's F1 pin exists for, and it is the case where a gate written as
// "warn when `!isFnSig`" and a gate written as "warn when the effective kind is not
// in `appliesTo`" happen to agree — so it is pinned to keep them agreeing after the
// next reshape of the effective-kind predicate.
TEST(SemanticAnalyzerC, AttributeDeclKindFunctionPointerObjectWarns) {
    auto m = analyzeShipped(
        "c",
        {"typedef int(*fp)(int);\n"
         "__attribute__((no_sanitize_thread)) fp g = 0;\n"
         "int main(void){return g == 0;}\n"});
    EXPECT_FALSE(m.hasErrors())
        << (m.diagnostics().all().empty()
                ? "" : m.diagnostics().all()[0].actual);
    EXPECT_EQ(ignoredForKindWarnings(m), 1u)
        << "a POINTER to function is an OBJECT — the attribute cannot apply to it, "
           "and `Ptr<FnSig>` is not `FnSig`";
    for (auto const& d : m.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_AttributeIgnoredForDeclarationKind)
            continue;
        EXPECT_NE(d.actual.find("variable"), std::string::npos) << d.actual;
    }
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        EXPECT_FALSE(m.symbols()[i].isNoSanitizeThread)
            << "…and nothing records the flag";
}

// ★★ THE SAME `Ptr<FnSig>` SHAPE, THE `warn_unused_result` SPELLING — AND HERE THE
// WARNING IS A **FALSE POSITIVE**. This test PINS A KNOWN DIVERGENCE FROM CLANG; it
// is NOT an assertion that the behaviour is correct.
//
//   [[D-CSUBSET-APPLIESTO-CANNOT-EXPRESS-FUNCTION-POINTER-OBJECT]]
//
// ⚠ READ THE CONTRAST WITH THE TEST DIRECTLY ABOVE BEFORE TOUCHING EITHER. Both feed
// a function-POINTER OBJECT (`dk == Variable`, `isFnSig == false`). They differ in the
// REFERENCE behaviour, MEASURED with `/usr/bin/clang -std=c23 -Wall -Wextra`:
//   * `no_sanitize_thread` on that shape → clang HARD-ERRORS ("'no_sanitize_thread'
//     attribute only applies to functions"), so the test above asserts a warning DSS
//     is RIGHT to emit;
//   * `warn_unused_result` on that shape → clang is **SILENT**, and it does not merely
//     tolerate the attribute, it HONORS it: through a call on such a pointer clang
//     warns `-Wunused-result` while DSS says nothing at the call. clang's own
//     applicability text — quoted verbatim in the shipped row — lists "function
//     pointers" among the valid positions.
// So DSS is wrong in BOTH directions on this one shape: a false positive at the
// declaration AND a false negative at the call. Both halves are pinned below.
//
// ⚠⚠ NOT FIXABLE BY WIDENING `appliesTo`, WHICH IS WHY THIS IS A PIN AND NOT A FIX.
// A function-pointer object is `DeclarationKind::Variable` — the SAME kind as the
// plain `int gw1 __attribute__((warn_unused_result)) = 1;` that clang genuinely DOES
// warn about (MEASURED, `-Wignored-attributes`). Adding `variable` to the row would
// silence the CORRECT case along with the incorrect one, so the 4-value
// `DeclarationKind` vocabulary cannot express clang's rule at all; closing it needs a
// 5th spelling (e.g. `functionPointer`). Asserting BOTH cases side by side in one test
// is what makes that inexpressibility OBSERVABLE rather than merely argued.
//
// ★ WHEN THE REGISTRY ROW CLOSES, **FLIP** THESE EXPECTATIONS — do not delete the
// test. A silent fix is as bad as a silent regression here: the whole point of the pin
// is that today's answer cannot change without a test turning red.
TEST(SemanticAnalyzerC,
     AttributeDeclKindFunctionPointerObjectWarnUnusedResultIsAKnownDivergence) {
    // The three spellings, each MEASURED clang-SILENT and DSS-loud.
    for (auto const* src : {
             "extern __attribute__((warn_unused_result)) int (*fp)(void);\n"
             "int main(void){ return 0; }\n",
             "extern int (*fp)(void) __attribute__((warn_unused_result));\n"
             "int main(void){ return 0; }\n",
             "extern __attribute__((__warn_unused_result__)) int (*fp)(void);\n"
             "int main(void){ return 0; }\n"}) {
        SCOPED_TRACE(src);
        auto m = analyzeShipped("c", {src});
        EXPECT_FALSE(m.hasErrors())
            << (m.diagnostics().all().empty()
                    ? "" : m.diagnostics().all()[0].actual);
        EXPECT_EQ(ignoredForKindWarnings(m), 1u)
            << "TODAY'S BEHAVIOUR, pinned as a KNOWN DIVERGENCE — clang compiles "
               "this silently AND honors the attribute. If this now reads 0 the "
               "divergence was CLOSED: update "
               "D-CSUBSET-APPLIESTO-CANNOT-EXPRESS-FUNCTION-POINTER-OBJECT and flip "
               "this expectation deliberately, rather than deleting the pin";
        for (auto const& d : m.diagnostics().all()) {
            if (d.code != DiagnosticCode::S_AttributeIgnoredForDeclarationKind)
                continue;
            EXPECT_NE(d.actual.find("variable"), std::string::npos) << d.actual;
        }
    }
    {   // THE FALSE-NEGATIVE HALF — clang warns AT THE CALL, DSS does not.
        auto m = analyzeShipped(
            "c",
            {"extern __attribute__((warn_unused_result)) int (*fp)(void);\n"
             "int main(void){ fp(); return 0; }\n"});
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_NodiscardResultDiscarded), 0u)
            << "TODAY'S BEHAVIOUR: the flag landed on a VARIABLE's SymbolRecord "
               "while `checkCall`'s discard check consults the CALLEE's, so the "
               "discarded result goes unreported — where clang warns "
               "-Wunused-result (MEASURED). Same root as the typedef miss below";
        EXPECT_EQ(ignoredForKindWarnings(m), 1u)
            << "…and the declaration still carries the false positive, so the two "
               "halves of this divergence are independent and both must be flipped";
    }
    {   // THE CASE THAT MAKES WIDENING THE ROW IMPOSSIBLE: a PLAIN data object is
        // the SAME DeclarationKind, and there the warning is CORRECT.
        auto m = analyzeShipped(
            "c",
            {"int gw1 __attribute__((warn_unused_result)) = 1;\n"
             "int main(void){ return gw1; }\n"});
        EXPECT_EQ(ignoredForKindWarnings(m), 1u)
            << "clang warns -Wignored-attributes here (MEASURED), so THIS warning "
               "is right — and it is `DeclarationKind::Variable`, exactly like the "
               "function-pointer object above. That is the inexpressibility: one "
               "kind, two required answers, so no `appliesTo` value can be correct";
    }
}

// ★★ THE SECOND MISS ON THE SAME ROOT — `warn_unused_result` ON A TYPEDEF IS NOT
// PROPAGATED TO THE CALL SITE. Also a PIN ON A KNOWN DIVERGENCE, not an endorsement.
//
//   [[D-CSUBSET-APPLIESTO-CANNOT-EXPRESS-FUNCTION-POINTER-OBJECT]] (second miss)
//
// The shipped row declares `type` in its `appliesTo`, which correctly STOPS the
// decl-kind gate from firing on a typedef — clang accepts every typedef shape at the
// DECLARATION. But `appliesTo` only silences the gate; it does not ROUTE the fact.
// RE-MEASURED 2026-07-30, `/usr/bin/clang -std=c23 -Wall -Wextra` against the shipped
// CLI, a DISCARDED call in each of the four shapes — and it is a 3-of-4 agreement, NOT
// the 4-of-4 the shipped row's own comment claimed until this cycle corrected it:
//   (1) `typedef __attribute__((warn_unused_result)) int T;` + `T g(void)` + `g();`
//       → clang WARNS ("ignoring return value of type 'T' …" [-Wunused-value]) while
//         DSS emits NOTHING.  ← THE DIVERGENCE
//   (2) leading fn-pointer typedef   → clang silent, DSS silent.  AGREEMENT
//   (3) trailing fn-pointer typedef  → clang silent, DSS silent.  AGREEMENT
//   (4) function-type typedef        → clang silent, DSS silent.  AGREEMENT
// Shape (1) is the one where the typedef names the RETURN TYPE rather than the callee:
// clang propagates the attribute from the return type onto the call expression, DSS
// does not, because the flag lands on the TYPE's record while `checkCall` reads the
// CALLEE's. Same root as the function-pointer-object miss above.
//
// ★ THE CONTROL IS WHAT MAKES THIS NON-VACUOUS. The DIRECT function form DOES warn
// (`warning[S003E]` S_NodiscardResultDiscarded, MEASURED, matching clang), so the
// discard checker is demonstrably ALIVE and shape (1)'s silence is specifically a
// missing ALIAS propagation — not a dead check that would make every `0u` below pass
// for the wrong reason.
//
// ★ WHEN THIS CLOSES, FLIP shape (1) to 1u; the three AGREEMENT shapes must STAY 0u,
// or the fix over-fired into C that every real toolchain compiles clean.
TEST(SemanticAnalyzerC,
     AttributeDeclKindTypedefReturnTypePropagationIsAKnownDivergence) {
    {   // THE CONTROL — the direct function form, where DSS and clang agree.
        auto m = analyzeShipped(
            "c",
            {"__attribute__((warn_unused_result)) int g(void){return 1;}\n"
             "int main(void){ g(); return 0; }\n"});
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_NodiscardResultDiscarded), 1u)
            << "the discard checker must be ALIVE, or every 0u below passes for the "
               "wrong reason and this whole test becomes decoration";
    }
    {   // SHAPE (1) — the divergence.
        auto m = analyzeShipped(
            "c",
            {"typedef __attribute__((warn_unused_result)) int T;\n"
             "T g(void){return 1;}\n"
             "int main(void){ g(); return 0; }\n"});
        EXPECT_FALSE(m.hasErrors())
            << (m.diagnostics().all().empty()
                    ? "" : m.diagnostics().all()[0].actual);
        EXPECT_EQ(ignoredForKindWarnings(m), 0u)
            << "the DECLARATION is correctly silent — clang accepts the typedef "
               "position for the GNU spelling, which is exactly what `type` in "
               "`appliesTo` buys";
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_NodiscardResultDiscarded), 0u)
            << "TODAY'S BEHAVIOUR, pinned as a KNOWN DIVERGENCE: clang WARNS here "
               "('ignoring return value of type T', MEASURED) and DSS does not — it "
               "does not propagate the attribute through the alias. If this now "
               "reads 1 the miss was CLOSED: update the registry row and flip this";
    }
    // SHAPES (2)–(4) — clang-SILENT and DSS-silent. Pinned as AGREEMENT so a future
    // propagation fix cannot over-fire into C every real toolchain accepts.
    for (auto const* src : {
             "typedef __attribute__((warn_unused_result)) int (*FP)(void);\n"
             "extern FP fp;\nint main(void){ fp(); return 0; }\n",
             "typedef int (*FP)(void) __attribute__((warn_unused_result));\n"
             "extern FP fp;\nint main(void){ fp(); return 0; }\n",
             "typedef int FT(void) __attribute__((warn_unused_result));\n"
             "extern FT g;\nint main(void){ g(); return 0; }\n"}) {
        SCOPED_TRACE(src);
        auto m = analyzeShipped("c", {src});
        EXPECT_EQ(ignoredForKindWarnings(m), 0u)
            << "clang accepts these typedef shapes silently (MEASURED) — a warning "
               "here is the false positive the row split exists to prevent";
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_NodiscardResultDiscarded), 0u)
            << "and clang does NOT warn at the call in these three, so DSS's "
               "silence is AGREEMENT — a propagation fix must not fire here";
    }
}

// ★★ ONE DIAGNOSTIC PER OFFENDING SPELLING, not one per DECLARATOR — AND READ THE
// CAVEAT, because half of this test is NOT a pin on the gate's latch.
//
// The gate sits inside the per-declarator loop while the declaration-level attribute
// facts are folded ONCE and COPIED into every declarator, so `int a, b, c;` reaches
// `report()` three times and the gate carries a clause-node latch for it.
//
// ⚠ MEASURED, AND IT REFUTES THE OBVIOUS RED-ON-DISABLE: deleting that latch leaves
// the FIRST case below GREEN. `DiagnosticReporter`'s `dedupWindow` (default 4) drops
// a report whose (code, buffer, span, ruleContext, `actual`) matches a recent one,
// and all three repeats match exactly. So case 1 pins the OBSERVABLE property —
// which is what a user experiences, and what a future change to either mechanism
// must preserve — not the latch specifically.
//
// ★ AND THE WINDOW IS **ALWAYS** 4 ON THIS PATH — do not reach for the "some driver
// sets `dedupWindow = 0`" argument, which an earlier write-up used and which is
// FALSE for this diagnostic: it reports into `EngineState::reporter`, a
// default-constructed reporter that `analyzeImpl` never reconfigures and then moves
// whole into the `SemanticModel`. The latch is kept for the OTHER reason — the dedup
// key includes `actual`, so the day this message names the declarator the window
// stops collapsing the repeats. See the gate's own comment.
//
// ★ CASE 2 **DOES** DISCRIMINATE, and it is what rules out the cheaper fix: a bare
// bool latch would collapse TWO DIFFERENT misplaced attributes on one declaration
// into one diagnostic — a fresh silent drop in the cycle whose whole purpose is to
// remove one — and the reporter's dedup would NOT have masked it (differing `actual`
// ⇒ differing key). Keying on the clause node is what makes both cases hold at once.
TEST(SemanticAnalyzerC, AttributeDeclKindWarnsOncePerSpellingNotPerDeclarator) {
    {   // THREE declarators, ONE spelling ⇒ ONE diagnostic.
        auto m = analyzeShipped(
            "c",
            {"__attribute__((noinline)) int a, b, c;\n"
             "int main(void){return a+b+c;}\n"});
        EXPECT_EQ(ignoredForKindWarnings(m), 1u)
            << "one erroneous specifier is one mistake — three declarators must "
               "not mean three diagnostics. (Held by the gate's clause-node latch "
               "AND, independently, by the reporter's dedup window — see the "
               "measured caveat above; this asserts the OBSERVABLE property)";
    }
    {   // ONE declarator, TWO distinct spellings ⇒ TWO diagnostics.
        auto m = analyzeShipped(
            "c",
            {"__attribute__((noinline)) __attribute__((warn_unused_result)) "
             "int gv = 1;\nint main(void){return gv;}\n"});
        EXPECT_EQ(ignoredForKindWarnings(m), 2u)
            << "two DIFFERENT misplaced attributes are two mistakes — a bare "
               "bool latch would swallow the second, which is exactly the silent "
               "drop this cycle exists to remove";
    }
}

// ★★ POSITIONS THE GATE COVERS THAT THE PLAN EXPECTED IT TO MISS — STATED BECAUSE
// SILENTLY EXCEEDING A SPEC IS AS HARD TO REVIEW AS SILENTLY MISSING IT.
//
// The plan listed block-scope locals as out of scope, on the reading that `varDecl`
// ignores `attrSpec`/`stdAttr` wholesale. MEASURED with the shipped CLI: that
// wholesale ignore belongs to the LINKAGE scan (`linkageSpecifierIgnoredRules`)
// only — the SEMANTIC attribute scan runs on block-scope declarations in BOTH the
// leading and the mid positions, so both WARN. That is strictly more fail-loud than
// planned and is pinned here so it cannot regress into the predicted silence
// unnoticed.
//
// STILL out of scope, and genuinely so: a struct member in the LEADING attribute
// position does not PARSE (P0009), so there is no declaration for the gate to judge.
TEST(SemanticAnalyzerC, AttributeDeclKindBlockScopeLocalAlsoWarns) {
    for (auto const* src : {
             "int main(void){ __attribute__((noinline)) int x = 1; return x; }\n",
             "int main(void){ int __attribute__((noinline)) x = 1; return x; }\n"}) {
        SCOPED_TRACE(src);
        auto m = analyzeShipped("c", {src});
        EXPECT_FALSE(m.hasErrors())
            << (m.diagnostics().all().empty()
                    ? "" : m.diagnostics().all()[0].actual);
        EXPECT_EQ(ignoredForKindWarnings(m), 1u)
            << "MEASURED: block scope reaches the semantic attribute scan in both "
               "positions, so both warn — the plan predicted silence here";
    }
}

// TF-C92: the NEGATIVE control for the symbol-tier suite — an un-annotated program
// must leave every record clear. Without it, an apply that unconditionally set the
// flag would satisfy the OR-merge test above.
//
// ★ AND THE COMPOSITION CLAIM, in the same test because it is the same shape: this
// axis contradicts NOTHING, so `no_sanitize_thread` together with `noinline` (and
// with `always_inline`) must be clean — no `S_ConflictingInlineAttributes`, unlike
// the inline pair. A future contradiction gate written by analogy to that pair would
// turn this red, which is the intent.
TEST(SemanticAnalyzerC, NoSanitizeThreadComposesAndDefaultsClear) {
    {
        auto model = analyzeShipped(
            "c",
            {"static int f(int x){return x+1;}\nint main(void){return f(41);}\n"});
        EXPECT_FALSE(model.hasErrors());
        for (std::size_t i = 1; i < model.symbols().size(); ++i)
            EXPECT_FALSE(model.symbols()[i].isNoSanitizeThread)
                << "an un-annotated program records no exclusion";
    }
    for (auto const* src : {
             "static __attribute__((no_sanitize_thread)) __attribute__((noinline)) "
             "int f(int x){return x+1;}\nint main(void){return f(41);}\n",
             "static __attribute__((no_sanitize_thread)) "
             "__attribute__((always_inline)) "
             "int f(int x){return x+1;}\nint main(void){return f(41);}\n"}) {
        SCOPED_TRACE(src);
        auto model = analyzeShipped("c", {src});
        EXPECT_FALSE(model.hasErrors())
            << "no_sanitize_thread composes with either inline directive — it is "
               "an orthogonal axis, not a third member of a mutually exclusive set: "
            << (model.diagnostics().all().empty()
                    ? "" : model.diagnostics().all()[0].actual);
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_ConflictingInlineAttributes), 0u);
        int marked = 0;
        for (std::size_t i = 1; i < model.symbols().size(); ++i)
            if (model.symbols()[i].name == "f"
                && model.symbols()[i].isNoSanitizeThread) ++marked;
        EXPECT_EQ(marked, 1)
            << "and the exclusion is still recorded alongside the inline directive";
    }
}

// (g) FC16 (D-CSUBSET-NORETURN): a shipped-descriptor symbol declared
// `"noreturn": true` (the abort/exit shape) threads onto the injected
// SymbolRecord's isNoreturn — a shipped extern has no user prototype to carry
// `_Noreturn`. Witnesses ShippedSymbol.noreturn -> SymbolRecord.isNoreturn at the
// injection site; a sibling symbol without the key stays non-noreturn.
// RED-ON-DISABLE: drop `rec.isNoreturn = sym.noreturn` at injection -> `boom`
// stays false.
TEST(SemanticAnalyzerC, NoreturnShippedDescriptorSymbolIsNoreturn) {
    dss::test_support::ScratchDir sysDir{
        dss::test_support::Location::Temp, "nr-desc"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "boom.json",
        R"({ "header": "boom.h", "library": { "pe": "msvcrt.dll", "elf": "libc.so.6" },
             "symbols": [ { "name": "boom",  "signature": "fn() -> void", "noreturn": true },
                          { "name": "plain", "signature": "fn() -> void" } ] })",
        "#include <boom.h>\nint main() { return 0; }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, BlockScopePrototypeMergesWithFileDefinition) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, BlockScopePrototypeAfterDefinitionNoUnusedWarning) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, BlockScopePrototypeEnablesForwardMutualCall) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, BlockScopePrototypeIncompatibleWithFileDefFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ExternFunctionThenDefinitionMerges) {
    auto model = analyzeShipped("c", {
        "extern int f(int);\n"
        "int f(int x){ return x; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "an extern function declaration + a definition must merge cleanly";
    EXPECT_EQ(countSurvivingFns(model, "f"), 1u);
}

// (b) Definition FIRST, then a redundant `extern` function declaration: also a
// clean merge (the definition keeps the binding; the extern is absorbed).
TEST(SemanticAnalyzerC, ExternFunctionAfterDefinitionMerges) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ExternObjectThenDefinitionMerges) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ExternObjectAfterDefinitionMerges) {
    auto model = analyzeShipped("c", {
        "int g = 6;\n"
        "extern int g;\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a definition followed by a redundant extern declaration must merge";
    EXPECT_EQ(countSurvivingSymbols(model, "g"), 1u);
}

// (e) extern idempotence: multiple extern declarations + one definition is well-
// formed (zero diagnostics, one surviving symbol).
TEST(SemanticAnalyzerC, ExternObjectIdempotentThenDefinition) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ExternObjectIncompatibleDefinitionFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, TwoObjectDefinitionsStillCollide) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ExternFunctionVsObjectCrossCategoryCollides) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, TypedefVsExternObjectCrossCategoryCollides) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, TypedefVsExternFunctionCrossCategoryCollides) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ExternObjectThenTypedefCrossCategoryCollides) {
    auto model = analyzeShipped("c", {
        "extern int g;\n"
        "typedef int g;\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "extern object (Variable) then typedef (Type) of the same name — "
           "different categories, must collide in either order";
}

// ── TF-C97 D-CSUBSET-REPEAT-TYPEDEF-SAME-TYPE (C11 6.7p3) — "a typedef name may be
//    redefined to denote the same type as it currently does, provided that type is
//    not a variably modified type". A typedef is neither a proto nor an extern nor
//    a tentative, so BOTH sides rank as definitions and the Pass-1 merge gate
//    (`!bothDefinitions`) rejected even a LEGAL repeat as S_RedeclaredSymbol. The
//    fix admits the Type↔Type both-definitions pair into the merge and VERIFIES it
//    after Pass 1.5 against the two RESOLVED TypeIds — never against spellings.

// Every SymbolRecord named `name` whose kind is Type — absorbed OR surviving. The
// C11 rule is about the two DECLARATIONS agreeing, so the assertions below must be
// able to see BOTH records, which `countSurvivingSymbols` (absorbed-filtering) hides.
[[nodiscard]] inline std::vector<SymbolRecord const*>
typedefRecords(SemanticModel const& model, std::string_view name) {
    std::vector<SymbolRecord const*> out;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        auto const& r = model.symbols()[i];
        if (r.name == name && r.kind == DeclarationKind::Type) out.push_back(&r);
    }
    return out;
}

// (1) THE RULE, positive: an IDENTICAL same-scope repeat is legal C11 and must
// analyze clean — AND the two declarations must resolve to the SAME TypeId, which
// is the property the whole merge exists to guarantee (a clean analysis with two
// unrelated types would be a silent miscompile waiting at the first use site).
// RED-ON-DISABLE: drop `|| typedefRepeat` from the Pass-1 merge gate and the pair
// falls into the bothDefinitions collision arm -> this S_RedeclaredSymbol count
// becomes 1 and the EXPECT_FALSE(hasErrors) flips red. MEASURED, not predicted.
TEST(SemanticAnalyzerC, RepeatTypedefSameTypeIsAccepted) {
    auto model = analyzeShipped("c", {
        "typedef struct S SS;\n"
        "typedef struct S SS;\n"
        "struct S { int a; };\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "C11 6.7p3 permits a typedef name to be redefined to the same type";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 0u);
    auto const recs = typedefRecords(model, "SS");
    ASSERT_EQ(recs.size(), 2u) << "both typedef declarations must mint a record";
    ASSERT_TRUE(recs[0]->type.valid());
    ASSERT_TRUE(recs[1]->type.valid());
    EXPECT_EQ(recs[0]->type.v, recs[1]->type.v)
        << "the two spellings of one typedef must resolve to the SAME TypeId — "
           "identical TypeIds are what makes the merge sound";
}

// (2) THE REGRESSION WITNESS — the real macOS SDK shape, and the reason this rule
// had to close for the arm64-macho sqlite leg to reach zero. Two DIFFERENT headers
// typedef ONE tag: `$SDK/usr/include/malloc/_malloc_type.h` names the
// forward-declared `struct _malloc_zone_t`, then `malloc/malloc.h` typedefs the
// SAME tag AT ITS COMPLETION (`typedef struct _malloc_zone_t { … } malloc_zone_t;`).
// After the preprocessor flattens the TU they are two same-scope typedefs of one
// name — MEASURED in sqlite's mem1.c, and MEASURED again here by extracting the two
// declarations verbatim from `clang -E` output. A synthetic-only test would miss
// this: the two spellings differ (one is a bare tag reference, the other completes
// the tag), so only comparing RESOLVED types can accept it.
// RED-ON-DISABLE: revert the Pass-1 admission -> exactly one S0002 on
// `malloc_zone_t`, which is the sqlite failure this closes.
TEST(SemanticAnalyzerC, RepeatTypedefAcrossHeadersCompletingTagIsAccepted) {
    auto model = analyzeShipped("c", {
        // "header A" — the tag is forward-declared, then aliased.
        "struct _mz_t;\n"
        "typedef struct _mz_t mz_t;\n"
        // "header B" — the SAME tag, completed, aliased again under one name.
        "typedef struct _mz_t { int version; int size; } mz_t;\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "the malloc_zone_t shape — one tag, two headers — is legal C11";
    auto const recs = typedefRecords(model, "mz_t");
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(recs[0]->type.v, recs[1]->type.v)
        << "the forward-declared tag and its completion are ONE type — the alias "
           "minted before the body must not be a second, distinct type";
}

// (3) THE RULE, negative: two DIFFERENT types under one typedef name stay LOUD.
// The specific code is S_IncompatibleRedeclaration — the deferred post-1.5 verdict
// the merged-decl sweep already emits for every other type-mismatched merge pair —
// NOT a vague "some error". RED-ON-DISABLE: drop the post-1.5 Type↔Type arm and the
// pair merges silently (count 0) with the FIRST type winning every later use.
TEST(SemanticAnalyzerC, RepeatTypedefDifferentTypeFailsLoud) {
    auto model = analyzeShipped("c", {
        "typedef int T;\n"
        "typedef long T;\n",
    });
    EXPECT_TRUE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 1u)
        << "`typedef int T; typedef long T;` is a real C11 error and must stay one";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "exactly ONE diagnostic for one mistake — not a collision AND a mismatch";
}

// (4) The negative that the OBJECT composite rule must not leak into: C 6.2.7 lets
// `extern char v[]; char v[3];` compose into the completed array, but 6.7p3 asks the
// strictly stronger "does it denote the SAME type". MEASURED against /usr/bin/clang:
// `typedef int T[3]; typedef int T[];` is `error: typedef redefinition with
// different types ('int[]' vs 'int[3]')`. RED-ON-DISABLE: let the Type↔Type pair
// fall through to the incomplete-array relaxation below it and this count drops to
// 0 — the mismatch is silently accepted.
TEST(SemanticAnalyzerC, RepeatTypedefIncompleteVsSizedArrayFailsLoud) {
    auto model = analyzeShipped("c", {
        "typedef int T[3];\n"
        "typedef int T[];\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 1u)
        << "a sized and an incomplete array are not the SAME type — the OBJECT "
           "composite-type relaxation must not reach a typedef repeat";
}

// (5) THE C11 CARVE-OUT: 6.7p3's permission excludes a VARIABLY MODIFIED type, so an
// IDENTICALLY spelled VLA typedef repeat is still invalid (C99 6.7.7p2 evaluates the
// size expression exactly once, AT the typedef — repeating the name would demand it
// twice). The interner has no length operand on a vlaArray, so the two TypeIds are
// EQUAL and a plain same-TypeId check would silently accept precisely the shape the
// standard singles out. MEASURED against /usr/bin/clang: `error: redefinition of
// typedef for variably-modified type 'int[n]'`. Reported as S_RedeclaredSymbol — the
// code this shape already produced from the Pass-1 collision, so the carve-out is
// byte-identical in CODE and only gains a message. RED-ON-DISABLE: drop the
// `variablyModified` term and the same-TypeId fast path accepts it — count 0.
TEST(SemanticAnalyzerC, RepeatTypedefVariablyModifiedFailsLoud) {
    auto model = analyzeShipped("c", {
        "int n = 4;\n"
        "int main(void) { typedef int V[n]; typedef int V[n]; return 0; }\n",
    });
    EXPECT_TRUE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "a variably-modified typedef may not be repeated, even identically";
}

// (6) The carve-out dominates: when the repeat is BOTH variably modified AND of a
// different type, the VM verdict is the one reported — the repeat is forbidden
// outright, which is a stronger statement than "these two differ". MEASURED against
// /usr/bin/clang, which reports `redefinition of typedef for variably-modified type
// 'long[n]'` (not its different-types diagnostic) for exactly this source.
TEST(SemanticAnalyzerC, RepeatTypedefVariablyModifiedAndDifferentReportsVM) {
    auto model = analyzeShipped("c", {
        "int n = 4;\n"
        "int main(void) { typedef int V[n]; typedef long V[n]; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "the variably-modified carve-out is tested first and wins";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 0u)
        << "one mistake, one diagnostic — never both verdicts";
}

// (7) SCOPE PIN, positive: an INNER-scope typedef SHADOWING an outer one is legal C
// and untouched — `mergeOrCollideRedeclaration` runs only when `s.scopes.bind`
// returns a prior in the SAME scope, so a shadow never reaches this rule at all.
// This guards that the admission did not widen scope handling: the two typedefs
// here differ in type, and the ONLY reason that is legal is the scope boundary.
TEST(SemanticAnalyzerC, TypedefShadowingInInnerScopeStaysLegal) {
    auto model = analyzeShipped("c", {
        "typedef int T;\n"
        "int main(void) { typedef long T; return (int)sizeof(T); }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 0u)
        << "a shadowing typedef in an INNER scope is not a redefinition";
}

// (8) SCOPE PIN, negative mirror: the SAME two typedefs in ONE (inner) scope ARE a
// redefinition and stay loud. Together with (7) this pins that the rule follows the
// SCOPE, not the nesting depth — the file-scope pair in (3) and this block-scope
// pair must behave identically.
TEST(SemanticAnalyzerC, RepeatTypedefDifferentTypeInBlockScopeFailsLoud) {
    auto model = analyzeShipped("c", {
        "int main(void) { typedef int T; typedef long T; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 1u)
        << "same-scope is same-scope — a block scope obeys 6.7p3 exactly as file "
           "scope does";
}

// (9) THE ADMISSION IS Type↔Type ONLY. A typedef repeating a NON-typedef name is a
// different KIND of symbol (clang: "redefinition of 'x' as different kind of
// symbol") and must still collide S_RedeclaredSymbol — the `category()` guard, not
// the definedness rank, is what rejects it, and the new gate is conjoined with
// `sameCategory` so it can never reach a cross-category pair. RED-ON-DISABLE: widen
// `typedefRepeat` to drop its `category(...) == Type` term and this becomes 0 — an
// object would be silently absorbed into a typedef.
TEST(SemanticAnalyzerC, ObjectThenTypedefStillCollidesAfterRepeatAdmission) {
    auto model = analyzeShipped("c", {
        "int x;\n"
        "typedef int x;\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 1u)
        << "an object and a typedef of one name are different kinds of symbol — "
           "the Type↔Type admission must not reach them";
}

// (10) A FUNCTION-TYPE typedef repeat is legal too (MEASURED: /usr/bin/clang accepts
// `typedef int F(void);` twice), and it must route through the SAME resolved-TypeId
// check rather than the sweep's FnSig-candidate arm — that arm is gated on the
// SURVIVOR being a real Function, which a typedef is not. Guards the interaction
// between this rule and D-CSUBSET-FN-TYPEDEF-PROTOTYPE.
TEST(SemanticAnalyzerC, RepeatTypedefFunctionTypeIsAccepted) {
    auto model = analyzeShipped("c", {
        "typedef int F(void);\n"
        "typedef int F(void);\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    auto const recs = typedefRecords(model, "F");
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(recs[0]->type.v, recs[1]->type.v)
        << "two identical function-type typedefs are ONE interned FnSig";
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
TEST(SemanticAnalyzerC, TentativeDefinitionThenDefinitionMerges) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, TwoTentativeDefinitionsMerge) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, StaticTentativeDefinitionThenDefinitionMerges) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, TwoRealDefinitionsStillCollide_Tentative) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, BlockScopeDuplicateNotTentativeStillCollides) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, TentativeDefinitionIncompatibleTypeFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ExternPlusDefinitionStillMerges_TentativeGuard) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, TypedefTagSameNameAsAliasNoCollision) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, TagAndAliasBothResolveSameType) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, UnknownTagByValueFiresIncompleteObject) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, TagAndOrdinaryObjectSameNameCoexist) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, MultiMemberPerSlotSuffixIsolated) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, MultiMemberHeadStarBindsPerDeclaratorNotShared) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, MultiMemberPerSlotBitfieldWidths) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, MultiMemberSingleVsCommaByteIdentical) {
    auto model = analyzeShipped("c", {
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

// (e) The sqlite3.c frontier shape, made buildable: a multi-declarator
// pointer PAIR (`*next, *prev`) sharing one head tag + a `void *data` + a
// trailing `int count`. (sqlite's HashElem points at ITSELF -- `struct HashElem
// *next`; an inline SELF-referential struct-tag pointer is a SEPARATE pre-
// existing limitation -- the tag is bound AFTER its body's fields are
// type-resolved in the post-order walk -- pinned as fail-loud below and tracked
// by D-CSUBSET-SELF-REFERENTIAL-STRUCT. Here the pointer target is a distinct
// already-defined tag, isolating the c23 multi-declarator behavior.) Four
// fields, correct per-slot types.
TEST(SemanticAnalyzerC, MultiMemberSqliteHashElemRepro) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, SelfReferentialStructCompiles) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, SelfByValueStructMemberFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, TypedefSelfReferentialStructCompiles) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, MutuallyRecursiveStructsCompile) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, MultiMemberUnionPerSlotSuffixIsolated) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, MultiMemberAnonymousBitfieldStillResolves) {
    auto model = analyzeShipped("c", {
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

TEST(SemanticAnalyzerC, MultiMemberDeclaresNothingStillLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AnonUnionMemberPromotesAndResolves) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AnonUnionMemberBesideNamedMember) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, NestedAnonMemberPromotes) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AnonMemberCollisionWithDirectFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AnonMemberNameMayShadowOuterIdentifier) {
    auto globalModel = analyzeShipped("c", {
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
    auto typedefModel = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AnonMemberAmbiguousSiblingFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AnonUnionWithInnerBitfieldNoFalseError) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, C25StructDefineMintsRefResolvesMemberTypes) {
    auto cu = buildShippedUnit("c", {
        "struct S { int x; };\n"
        "int main() { struct S v; int y; y = v.x; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, C25ForwardTypedefThenDefinitionResolves) {
    auto cu = buildShippedUnit("c", {
        "typedef struct Foo Foo;\n"
        "struct Foo { int x; };\n"
        "int main() { Foo v; int y; y = v.x; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, C25ForwardPointerFieldResolves) {
    auto cu = buildShippedUnit("c", {
        "struct A { struct B *b; };\n"
        "struct B { int x; };\n"
        "int main() { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, C35UndefinedStructTagByValueFailsLoudIncompleteObject) {
    auto cu = buildShippedUnit("c", {
        "int main() { struct Nope v; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompleteTypeObject), 1u)
        << "a by-value `struct Nope v;` (undefined tag) is an incomplete object — "
           "must fail loud S_IncompleteTypeObject";
}

// (3d) Redefinition of a tag collides — S_RedeclaredSymbol, exactly as today.
TEST(SemanticAnalyzerC, C25StructTagRedefinitionCollides) {
    auto cu = buildShippedUnit("c", {
        "struct S { int x; };\n"
        "struct S { int y; };\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, C35BareStructForwardDeclMintsIncompleteTag) {
    auto cu = buildShippedUnit("c", {
        "struct S;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, C35OpaqueTypedefViaPointerCompiles) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, C35ForwardThenDefineCompletesNoCollision) {
    auto model = analyzeShipped("c", {
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
// (tests/mir/test_mir_lowering_c.cpp), which runs the full pipeline.

// (c35 ★ fail-loud) MEMBER of an incomplete-pointer: `struct S; p->x` where S is
// incomplete — the member access has no layout to resolve and must FAIL LOUD
// (S_NotAComposite / the layout miss), never a wrong offset. RED-on-disable: a
// dropped incomplete guard would resolve a phantom offset.
TEST(SemanticAnalyzerC, C35MemberOfIncompletePointerFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, C35SizeofOfIncompleteFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "struct S;\n"
        "int a[sizeof(struct S)];\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_TRUE(model.hasErrors())
        << "sizeof of an incomplete struct must fail loud";
}

// (c35 PRESERVE) TWO DEFINITIONS still collide: `struct S { int a; }; struct S
// { int b; };` — two COMPLETE definitions of the same tag are a redefinition
// (S_RedeclaredSymbol). The forward-decl relaxation must NOT swallow this — only
// an INCOMPLETE prior tag is completable; a complete one collides. RED-on-disable:
// losing this lets a struct be silently redefined with a different layout.
TEST(SemanticAnalyzerC, C35TwoDefinitionsStillCollide) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, C25AnonymousTypedefStructDefinesAndResolves) {
    auto cu = buildShippedUnit("c", {
        "typedef struct { int x; } T;\n"
        "int main() { T v; int y; y = v.x; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors())
        << "anonymous typedef struct define + alias use + member access clean";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotAComposite), 0u);
}

// (3e) Nested inline-body field: `struct Outer { struct Inner { int x; } in; };`
// — the inner body is itself a definition (a structSpec WITH a structBody, nested
// as a field). Both compose. RED-on-disable: the recursive define path depends on
// the nested specifier being routed as a definition by its own body child.
TEST(SemanticAnalyzerC, C25NestedInlineBodyFieldComposes) {
    auto cu = buildShippedUnit("c", {
        "struct Outer { struct Inner { int x; } in; };\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, C25UnionDefineMintsRefResolves) {
    auto cu = buildShippedUnit("c", {
        "union U { int i; long l; };\n"
        "int main() { union U u; int y; y = u.i; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, C35UndefinedUnionTagByValueFailsLoudIncompleteObject) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, C25EnumDefineMintsEnumeratorsVisibleRefResolves) {
    auto cu = buildShippedUnit("c", {
        "enum E { A, B, C };\n"
        "int main() { enum E e; int y; y = B; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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

TEST(SemanticAnalyzerC, C25UndefinedEnumTagFailsLoudUnknownType) {
    auto model = analyzeShipped("c", {
        "int main() { enum Nope e; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 1u)
        << "a bare `enum Nope` (undefined tag) must fail loud S_UnknownType";
}

// ─────────────────────────────────────────────────────────────────────────
// c28 D-CSUBSET-LOCAL-TYPE-DEFINITION: a BLOCK-SCOPED struct/union/enum
// DEFINITION with NO declarator (`struct S { int a; };` as a STATEMENT inside
// a function — sqlite3.c walMergesort). The varDecl init-declarator-list
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
TEST(SemanticAnalyzerC, C28LocalStructDefineNodeShapeAndType) {
    auto cu = buildShippedUnit("c", {
        "int main(void){ struct S { int a; int b; }; struct S v; int y; "
        "y = v.a; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, C28LocalUnionAndEnumDefine) {
    auto cuU = buildShippedUnit("c", {
        "int main(void){ union U { int a; int b; }; union U v; int y; "
        "y = v.a; return 0; }\n",
    });
    assertNoBuilderErrors(*cuU);
    auto mU = analyze(cuU, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(mU.hasErrors()) << "a block-scoped union definition + ref must be clean";
    TypeId const u = composedAggregate(mU, "U");
    ASSERT_TRUE(u.valid());
    EXPECT_EQ(mU.lattice().interner().kind(u), TypeKind::Union);

    auto cuE = buildShippedUnit("c", {
        "int main(void){ enum E { A, B, C }; enum E e; int y; "
        "y = B; e = A; return 0; }\n",
    });
    assertNoBuilderErrors(*cuE);
    auto mE = analyze(cuE, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, C28LocalStructDoesNotLeakToOuterScope) {
    auto cu = buildShippedUnit("c", {
        "int main(void){ { struct S { int a; }; } struct S w; w.a = 1; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, C28InnerStructShadowsOuterDistinctType) {
    auto cu = buildShippedUnit("c", {
        "struct S { int a; };\n"
        "int main(void){ struct S { long b; long c; }; struct S v; "
        "(void)v; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
// rejecting it, this flips. (Pairs with HirLoweringC.LocalDeclaresNothing*.)
TEST(SemanticAnalyzerC, C28LocalIntSemicolonSemanticallyClean) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, C28LocalAbstractDeclaratorSingleDiagnostic) {
    auto model = analyzeShipped("c", {
        "int main(void){ int *; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeclarationDeclaresNothing), 1u)
        << "`int *;` (abstract declarator) is rejected ONCE at the semantic tier";
}

// (c28g) REGRESSION: making the list optional must NOT break the ordinary local
// declaration forms (declarator present). `int x;` / `int x, y;` / `static int x;`
// / `struct S { … } v;` still mint their symbols and stay clean.
TEST(SemanticAnalyzerC, C28OrdinaryLocalDeclsUnaffected) {
    auto model = analyzeShipped("c", {
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
// function (sqlite3.c `typedef void(*LOGFUNC_t)(void*,int,const char*);`).
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
TEST(SemanticAnalyzerC, C30LocalTypedefNodeShapeAndType) {
    auto cu = buildShippedUnit("c", {
        "int main(void){ typedef int (*FN_t)(int); FN_t f; (void)f; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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

// (c30b) The exact sqlite3.c frontier shape: a block-scoped fn-ptr typedef
// with a void return + (void*,int,const char*) params, then a local var of that
// type. Must be clean (no S_UnknownType for the in-block typedef-name use).
TEST(SemanticAnalyzerC, C30LocalTypedefFrontierShape) {
    auto cu = buildShippedUnit("c", {
        "int main(void){ typedef void (*LOGFUNC_t)(void*, int, const char*); "
        "LOGFUNC_t xLog; (void)xLog; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, C30LocalTypedefDoesNotLeakToOuterScope) {
    // CONTROL: the inner-block typedef + an IN-BLOCK use analyzes clean.
    auto okModel = analyzeShipped("c", {
        "int main(void){ { typedef int MyT; MyT a; (void)a; } return 0; }\n",
    });
    EXPECT_FALSE(okModel.hasErrors())
        << "the inner-block typedef + in-block use must analyze clean (control)";
    // LEAK PROBE: an OUTER use of the block-local name must fail S_UnknownType.
    auto leakModel = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, C30InnerTypedefShadowsOuterDistinctType) {
    auto cu = buildShippedUnit("c", {
        "typedef long MyT;\n"
        "int main(void){ typedef int MyT; MyT a; (void)a; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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
TEST(SemanticAnalyzerC, FlexibleArrayStructAsUnionMemberIsAccepted) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FlexibleArrayStructAsStructMemberStillRejected) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ArrayOfFlexibleArrayStructInUnionStillRejected) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, UnionContainingFamStructAsUnionMemberIsAccepted) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, NullptrInitsAndComparesPointer) {
    auto cu = buildShippedUnit("c", {
        "int f(void){ void *p = nullptr; int r = p == nullptr; p = nullptr;"
        " return r + (nullptr == p); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// `int x = nullptr;` is a constraint violation — nullptr converts only to pointers.
TEST(SemanticAnalyzerC, NullptrToIntFailsLoud) {
    auto cu = buildShippedUnit("c", { "int f(void){ int x = nullptr; return x; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u);
}

// `bool b = nullptr;` now CONVERTS to false — C23 6.3.2.3.2 (nullptr -> bool =
// false), realized once the general scalar->bool conversion landed
// (D-CSUBSET-NULLPTR-BOOL-CONVERSION closed via `scalarConvertsToBool`). Was a
// DEFERRED S_TypeMismatch; the [[D-CSUBSET-SIZEOF-COMPARISON-INT-TYPE]] fix (a
// comparison now types `int`) forced the general scalar->bool arm, and
// nullptr->bool falls out as the NullptrT case.
TEST(SemanticAnalyzerC, NullptrToBoolConverts) {
    auto cu = buildShippedUnit("c", { "int f(void){ bool b = nullptr; return 0; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "nullptr -> bool converts to false (C23 6.3.2.3.2), no longer deferred";
    EXPECT_FALSE(model.hasErrors());
}

// `bool b = 0;` (a scalar zero) CONVERTS to false via the general scalar->bool
// conversion (C 6.3.1.2). Pre-D-CSUBSET-NULLPTR-BOOL-CONVERSION this was itself an
// S_TypeMismatch (the c had NO scalar->bool path — the very inconsistency
// that kept nullptr->bool deferred); now both assign.
TEST(SemanticAnalyzerC, ScalarZeroToBoolConverts) {
    auto cu = buildShippedUnit("c", { "int f(void){ bool b = 0; return b; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "scalar 0 -> bool converts to false (C 6.3.1.2)";
    EXPECT_FALSE(model.hasErrors());
}

// The fail-loud operator gate: nullptr in arithmetic (`nullptr + 1`) is rejected —
// WITHOUT the gate the HIR lowering (nullptr → integer 0) would silently accept it.
TEST(SemanticAnalyzerC, NullptrArithmeticFailsLoud) {
    auto cu = buildShippedUnit("c", { "void *f(void){ return nullptr + 1; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 1u);
}

// The gate rejects a RELATIONAL comparison (`nullptr < p`) — only `==`/`!=` against a
// pointer/nullptr peer is admissible.
TEST(SemanticAnalyzerC, NullptrRelationalFailsLoud) {
    auto cu = buildShippedUnit("c", { "int f(void *p){ return nullptr < p; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 1u);
}

// `==` against a NON-pointer, NON-nullptr peer (`nullptr == 5`) is rejected.
TEST(SemanticAnalyzerC, NullptrEqualsIntFailsLoud) {
    auto cu = buildShippedUnit("c", { "int f(void){ return nullptr == 5; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 1u);
}

// The gate must NOT fire on a plain assignment `p = nullptr` (handled by isAssignable)
// — the false-positive that the Assign/Comma/compound classification fix closed.
TEST(SemanticAnalyzerC, NullptrPlainAssignNoFalsePositive) {
    auto cu = buildShippedUnit("c", {
        "int f(void *p){ p = nullptr; return p == nullptr; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// A COMPOUND assignment (`p += nullptr`) IS pointer arithmetic → rejected.
TEST(SemanticAnalyzerC, NullptrCompoundAssignFailsLoud) {
    auto cu = buildShippedUnit("c", { "int f(void *p){ p += nullptr; return 0; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 1u);
}

// unary `-nullptr` is rejected (Neg on nullptr is not a valid operand).
TEST(SemanticAnalyzerC, NullptrUnaryNegFailsLoud) {
    auto cu = buildShippedUnit("c", { "void *f(void){ return -nullptr; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 1u);
}

// `nullptr` passed as a VARIADIC argument is rejected (no default arg promotion).
TEST(SemanticAnalyzerC, NullptrVariadicArgFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "int g(int n, ...);\n"
        "int f(void){ return g(1, nullptr); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 1u);
}

// A conditional with a nullptr arm (`c ? nullptr : p`, nullptr FIRST) types as the
// pointer — the combineTernary NullptrT arm; without it the ternary would mistype as
// NullptrT and a `void*` return would then mismatch.
TEST(SemanticAnalyzerC, NullptrTernaryTypesAsPointer) {
    auto cu = buildShippedUnit("c", {
        "void *f(int c, void *p){ return c ? nullptr : p; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// A bare function designator / array name DECAYS to a pointer, so `nullptr == func`
// and `nullptr == arr` are valid C23 comparisons — the gate must NOT reject them.
// Regression pin for the Eq/Ne peer-decay fix (FnSig / Array peers, not just Ptr).
TEST(SemanticAnalyzerC, NullptrComparedToDesignatorsAdmitted) {
    auto cu = buildShippedUnit("c", {
        "int g(void);\n"
        "int f(void){ int a[4]; if (nullptr == g) return 1;"
        " if (nullptr == a) return 2; return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NullptrInvalidOperand), 0u);
}

// sizeof(nullptr) folds to the pointer width (C23: sizeof(nullptr_t) == sizeof(void*))
// via the scalarByteSize NullptrT arm — a regression dropping the arm makes the fold
// fail (nullopt → error). Pins that it stays well-typed (size_t / U64).
TEST(SemanticAnalyzerC, NullptrSizeofIsWellTyped) {
    auto cu = buildShippedUnit("c", {
        "unsigned long f(void){ return sizeof(nullptr); }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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

TEST(SemanticAnalyzerC, EnumExplicitUnderlyingTypeSetsScalars) {
    auto cu = buildShippedUnit("c", {
        "enum E : unsigned char { A, B };\n"
        "int main(void) { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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

TEST(SemanticAnalyzerC, EnumUnderlyingTypeDistinctFromDefault) {
    auto cu = buildShippedUnit("c", {
        "enum Wide { WA };\n"                     // default int
        "enum Narrow : unsigned char { NA };\n"   // explicit u8
        "int main(void) { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
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

TEST(SemanticAnalyzerC, EnumAnonymousWithExplicitUnderlying) {
    auto cu = buildShippedUnit("c", {
        "enum : unsigned char { AX, AY };\n"      // anonymous + explicit underlying
        "int main(void) { return AY; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors())
        << "an anonymous enum with an explicit underlying type must be clean";
    auto const& ti = model.lattice().interner();
    TypeId enumTy{};
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "AX") enumTy = model.symbols()[i].type;
    ASSERT_TRUE(enumTy.valid());
    EXPECT_EQ(ti.scalars(enumTy)[0], static_cast<std::int64_t>(TypeKind::U8));
}

TEST(SemanticAnalyzerC, EnumUnderlyingNonIntegerFailsLoud) {
    auto model = analyzeShipped("c", {
        "enum E : float { A };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidEnumUnderlyingType), 1u)
        << "a float underlying type must fail loud S_InvalidEnumUnderlyingType";
}

TEST(SemanticAnalyzerC, EnumUnderlyingStructFailsLoud) {
    auto model = analyzeShipped("c", {
        "struct Foo { int x; };\n"
        "enum E : struct Foo { A };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidEnumUnderlyingType), 1u)
        << "a struct underlying type must fail loud S_InvalidEnumUnderlyingType";
}

// TF-C101 — the pin that keeps `enumUnderlyingBase` SEPARATE from `castTypeBase`.
// The clause's base is followed immediately by the enumerator-list `{`, so it must
// stay REF-ONLY: routing it through the definition-admitting type-name base makes
// `structSpec`'s greedy `{optional structBody}` swallow `{ A }`, the struct body
// then desyncs on `A`, the speculative clause rolls back whole, and the single
// precise diagnostic above becomes a nine-entry parse cascade (MEASURED). This test
// is the TYPEDEF spelling of the same constraint violation — it reaches the check
// through the Identifier arm, with no brace binding in play — so the two together
// pin the constraint from both directions and neither can be satisfied by a parse
// error standing in for the analysis.
TEST(SemanticAnalyzerC, EnumUnderlyingTypedefStructFailsLoud) {
    auto model = analyzeShipped("c", {
        "struct Foo { int x; };\n"
        "typedef struct Foo FooT;\n"
        "enum E : FooT { A };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidEnumUnderlyingType), 1u)
        << "a typedef'd struct underlying type must fail loud by CONSTRAINT, at the "
           "semantic tier — not as a parse cascade";
}

TEST(SemanticAnalyzerC, EnumeratorValueOutOfRangeFailsLoud) {
    auto model = analyzeShipped("c", {
        "enum E : unsigned char { A = 256 };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_EnumeratorValueOutOfRange), 1u)
        << "256 does not fit unsigned char (max 255) — must fail loud";
}

TEST(SemanticAnalyzerC, EnumeratorValueAtBoundaryClean) {
    auto model = analyzeShipped("c", {
        "enum E : unsigned char { A = 255 };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_EnumeratorValueOutOfRange), 0u)
        << "255 is exactly the unsigned-char max — in range, no error";
    EXPECT_FALSE(model.hasErrors());
}

TEST(SemanticAnalyzerC, EnumDefaultUnderlyingRangeCheckNeverFires) {
    // A default-int enum is NEVER range-checked (hasExplicitUnderlying == false),
    // so a value that would overflow a narrow type but fits int is clean — the
    // C-classic behavior is unchanged.
    auto model = analyzeShipped("c", {
        "enum E { A = 256, B = 70000 };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_EnumeratorValueOutOfRange), 0u)
        << "a default-int enum range check must NEVER fire";
    EXPECT_FALSE(model.hasErrors());
}

TEST(SemanticAnalyzerC, EnumBitfieldWidthValidatesAgainstExplicitUnderlying) {
    // A bit-field of an enum with an EXPLICIT unsigned-char underlying validates
    // its width against 8 bits (the existing bit-field check reads the enum
    // underlying via enumUnderlyingOrSelf, scalars[0] = U8): width 9 exceeds it
    // and fails loud; width 8 is exactly in range.
    auto bad = analyzeShipped("c", {
        "enum E : unsigned char { A };\n"
        "struct S { enum E f : 9; };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(bad.diagnostics(),
                        DiagnosticCode::S_BitFieldWidthOutOfRange), 1u)
        << "width 9 > the U8 underlying 8 bits must fail loud";
    auto ok = analyzeShipped("c", {
        "enum E : unsigned char { A };\n"
        "struct S { enum E f : 8; };\n"
        "int main(void){ return 0; }\n",
    });
    EXPECT_EQ(countCode(ok.diagnostics(),
                        DiagnosticCode::S_BitFieldWidthOutOfRange), 0u)
        << "width 8 == the U8 underlying 8 bits is exactly in range";
}

TEST(SemanticAnalyzerC, EnumAnonymousBitfieldColonSurvivesSpeculativeOptional) {
    // THE FORK PIN (Option B): the pre-existing anonymous enum-typed struct
    // bit-field `enum Color : 3;` must STILL parse after adding the speculative
    // underlying-type clause — the speculative optional TRIES `: <type>` and
    // ROLLS BACK to the bit-field reading when an int-constant (not a type)
    // follows the colon. A plain non-speculative optional (Option A) would break
    // this with a loud parse error.
    auto model = analyzeShipped("c", {
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

TEST(SemanticAnalyzerC, EnumUnderlyingBareIdentifierWidthRollsBackToBitfield) {
    // The requireKnownType triage pin: `enum Color : W3` where W3 is a VALUE
    // identifier (an enum constant, NOT a type) rolls the speculative
    // underlying-type clause back so `: W3` is the anonymous bit-field width.
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ConstexprDeltaVsConst) {
    auto constModel = analyzeShipped("c", {
        "int main(int argc, char **argv) { const int x = argc; return x; }\n",
    });
    EXPECT_FALSE(constModel.hasErrors())
        << "`const int x = argc;` is legal C — const is initializer-blind";
    auto cxModel = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ConstexprFoldsAndMarksSymbol) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ConstexprCharAndBoolFold) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, StaticAssertCharLiteralConditionFolds) {
    auto model = analyzeShipped("c", {
        "_Static_assert('a' == 97, \"char folds\");\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "'a' is an integer character constant (value 97) — must fold";
    EXPECT_FALSE(model.hasErrors());
}

TEST(SemanticAnalyzerC, StaticAssertTrueKeywordConditionFolds) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FloatDoesNotLeakIntoIntegerConstExprConsumers) {
    auto saModel = analyzeShipped("c", {
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
    auto dimModel = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, NullptrStaysNonFoldableInIntegerConstExpr) {
    // VLA C1a (D-CSUBSET-VLA): pinned at FILE scope — `nullptr` is not an integer
    // constant, so the file-scope array stays S_NonConstantArrayLength (not a VLA).
    auto dimModel = analyzeShipped("c", {
        "int a[nullptr];\n",
    });
    EXPECT_EQ(countCode(dimModel.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 1u);
    auto saModel = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ConstexprFunctionFormsFailLoud) {
    auto protoModel = analyzeShipped("c", {
        "constexpr int f(void);\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(protoModel.diagnostics(),
                        DiagnosticCode::S_ConstexprFunctionNotSupported), 1u)
        << "a constexpr function PROTOTYPE must fail loud";
    auto defModel = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ConstexprMissingInitializerFiresPerDeclarator) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ConstexprInForInitAccepted) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ConstexprVolatileObjectRejectedPointeeLegal) {
    auto badModel = analyzeShipped("c", {
        "int main(void) { constexpr volatile int v = 1; return v; }\n",
    });
    EXPECT_EQ(countCode(badModel.diagnostics(),
                        DiagnosticCode::S_ConstexprInvalidQualifier), 1u)
        << "a volatile-qualified constexpr OBJECT violates 6.7.1p11";
    auto okModel = analyzeShipped("c", {
        "int main(void) { constexpr volatile int *p = nullptr; "
        "return p == 0 ? 0 : 1; }\n",
    });
    EXPECT_EQ(countCode(okModel.diagnostics(),
                        DiagnosticCode::S_ConstexprInvalidQualifier), 0u)
        << "a volatile POINTEE is legal — the constexpr object is the pointer";
    EXPECT_FALSE(okModel.hasErrors());
    auto eastModel = analyzeShipped("c", {
        "int main(void) { constexpr int * volatile p = nullptr; return 0; }\n",
    });
    EXPECT_EQ(countCode(eastModel.diagnostics(),
                        DiagnosticCode::S_ConstexprInvalidQualifier), 1u)
        << "east `int * volatile p` IS a volatile-qualified object — rejected";
}

// F5 — an ENUM-typed constexpr is admitted as arithmetic: the enumerator
// initializer folds through the shared resolveSymbolValue direct-value arm.
TEST(SemanticAnalyzerC, ConstexprEnumTypedAdmitted) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ConstexprFloatFolds) {
    auto model = analyzeShipped("c", {
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
// constant here (fail loud).
TEST(SemanticAnalyzerC, ConstexprPointerNullFormsAcceptedAddressRejected) {
    auto okModel = analyzeShipped("c", {
        "constexpr int *p1 = nullptr;\n"
        "constexpr int *p2 = 0;\n"
        "int main(void) { return p1 == p2 ? 0 : 1; }\n",
    });
    EXPECT_FALSE(okModel.hasErrors())
        << "nullptr and integer-0 are null pointer constants (C 6.3.2.3p3)";
    auto badModel = analyzeShipped("c", {
        "int g;\n"
        "constexpr int *p = &g;\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(badModel.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 1u)
        << "&g is an address constant, not a supported constexpr pointer init";
}

// P33 (D-CSUBSET-CONSTEXPR-POINTER-CAST-NULL, C 6.3.2.3p3 + 6.6): the CAST-form
// null pointer constant initializes a constexpr pointer object. gcc 13.3.0 and
// clang 19.1.1 both accept every accepted form here (MEASURED on WSL x86_64).
// RED-ON-DISABLE: delete the `constexprPointerCastFoldsToNull` call from
// `validateConstexprDeclarator`'s Ptr arm and all three accepted forms become
// S_ConstexprNonConstantInitializer.
TEST(SemanticAnalyzerC, ConstexprPointerCastNullFormsAccepted) {
    auto model = analyzeShipped("c", {
        "constexpr int *p1 = (int *)0;\n"
        "constexpr int *p2 = (void *)0;\n"
        "constexpr int *p3 = (int *)(2 - 2);\n"
        "int main(void) { return (p1 == p2 && p2 == p3) ? 0 : 1; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 0u)
        << "(T*)0 / (void*)0 / (T*)(ICE folding to 0) are constant null pointers";
}

// ★ THE WALL THE WIDENING MUST NOT BREACH. The registry row's first option was to
// widen the SHARED `admitsNullPointerConstant`, which five other call sites reach
// only after `isAssignable` has already FAILED — so widening it there would
// silently ADMIT incompatible-pointer conversions that both references diagnose.
// These pins prove the widening did NOT happen at those sites: a `(float*)0`
// initializer, argument and return still fail loud.
TEST(SemanticAnalyzerC, PointerCastNullWideningDidNotLeakToTheSharedSites) {
    auto initModel = analyzeShipped("c", {
        "int *p = (float *)0;\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(initModel.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
        << "decl-init keeps the incompatible-pointer diagnostic";
    auto argModel = analyzeShipped("c", {
        "int f(int *p);\n"
        "int main(void) { return f((float *)0); }\n",
    });
    EXPECT_EQ(countCode(argModel.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
        << "checkCall keeps the incompatible-pointer diagnostic";
    // ⚠ The return site spells its refusal S_ReturnTypeMismatch, NOT S_TypeMismatch
    // — ✔MEASURED: this assertion was written against the wrong code and the suite
    // caught it. `checkReturn` calls `admitsNullPointerConstant` and then
    // `emitMismatch`, which is the return-specific code.
    auto retModel = analyzeShipped("c", {
        "int *f(void) { return (float *)0; }\n"
        "int main(void) { return f() == 0 ? 0 : 1; }\n",
    });
    EXPECT_EQ(countCode(retModel.diagnostics(),
                        DiagnosticCode::S_ReturnTypeMismatch), 1u)
        << "checkReturn keeps the incompatible-pointer diagnostic";
}

// P33 (D-CSUBSET-CONSTEXPR-AGGREGATE-TYPE, C23 6.7.1): a constexpr ARRAY / STRUCT
// / UNION object is admitted when every element of its brace initializer folds.
// The char-array-from-string form rides the SAME walk — the UNIFORM boundary the
// row demanded, kept by NOT carving it out.
// RED-ON-DISABLE: restore the blanket
// `if (k == Array || k == Struct || k == Union) emit(S_ConstexprUnsupportedType)`
// and every arm here becomes S_ConstexprUnsupportedType.
TEST(SemanticAnalyzerC, ConstexprAggregateTypesValidateElementWise) {
    auto arrModel = analyzeShipped("c", {
        "constexpr int a[3] = {1, 2, 3};\n"
        "int main(void) { return a[0] + a[1] + a[2] - 6; }\n",
    });
    EXPECT_FALSE(arrModel.hasErrors()) << "every element is a constant";
    auto strModel = analyzeShipped("c", {
        "constexpr char s[] = \"hi\";\n"
        "int main(void) { return s[2]; }\n",
    });
    EXPECT_FALSE(strModel.hasErrors())
        << "the char-array-from-string form rides the same element walk";
    auto structModel = analyzeShipped("c", {
        "struct S { int x; int y; };\n"
        "constexpr struct S s = {1, 2};\n"
        "int main(void) { return s.x + s.y - 3; }\n",
    });
    EXPECT_FALSE(structModel.hasErrors());
    auto unionModel = analyzeShipped("c", {
        "union U { int a; };\n"
        "constexpr union U u = {5};\n"
        "int main(void) { return u.a - 5; }\n",
    });
    EXPECT_FALSE(unionModel.hasErrors());
    auto nestedModel = analyzeShipped("c", {
        "constexpr int m[2][2] = {{1, 2}, {3, 4}};\n"
        "int main(void) { return m[1][1] - 4; }\n",
    });
    EXPECT_FALSE(nestedModel.hasErrors()) << "a nested brace list recurses";
    auto ptrMemberModel = analyzeShipped("c", {
        "struct H { int *p; int tag; };\n"
        "constexpr struct H h = {(int *)0, 3};\n"
        "int main(void) { return h.p == 0 ? h.tag - 3 : 1; }\n",
    });
    EXPECT_FALSE(ptrMemberModel.hasErrors())
        << "the element walk and the top-level Ptr arm admit the same value forms";
}

// The REFUSING half — a constexpr aggregate whose initializer is not a
// compile-time constant still fails loud, per element. gcc 13.3.0 ("initializer
// element is not constant") and clang 19.1.1 ("must be initialized by a constant
// expression") refuse both of these (MEASURED).
TEST(SemanticAnalyzerC, ConstexprAggregateNonConstantElementFailsLoud) {
    auto elemModel = analyzeShipped("c", {
        "int main(int argc, char **argv) {\n"
        "    constexpr int a[3] = {1, argc, 3};\n"
        "    return a[0];\n"
        "}\n",
    });
    EXPECT_EQ(countCode(elemModel.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 1u)
        << "one diagnostic, at the offending ELEMENT";
    auto copyModel = analyzeShipped("c", {
        "struct S { int x; };\n"
        "struct S s0;\n"
        "int main(void) { constexpr struct S s = s0; return 0; }\n",
    });
    EXPECT_EQ(countCode(copyModel.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 1u)
        << "a copy from a non-constant object is not a constant initializer";
    auto addrModel = analyzeShipped("c", {
        "int g;\n"
        "struct H { int *p; };\n"
        "constexpr struct H h = {&g};\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(addrModel.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 1u)
        << "an address constant is refused one brace deep exactly as at top level";
}

// ★ THE ONE AGGREGATE SHAPE THAT STAYS S_ConstexprUnsupportedType. A VARIABLY
// MODIFIED type has no compile-time size, so no element walk can make it a
// constant object — gcc ("'constexpr' object has variably modified type") and
// clang ("constexpr variable cannot have type 'const int[argc]'") both refuse it.
//
// ⚠ THE FIXTURE MOVED IN P34, AND THE REASON IS THE POINT. This test used to use
// `constexpr int a[argc] = {1,2,3};` and assert S_ConstexprUnsupportedType,
// because the declarator resolver DROPPED the written `argc` bound and re-sized
// the object from the brace list — so the constexpr guard's declarator scan was
// the only thing that could see the variable modification. That dropped bound was
// itself the defect D-CSUBSET-VLA-INITIALIZER closed: the non-empty form is now a
// constraint violation (S_VlaInitializerNotEmpty) refused BEFORE this validator
// runs, exactly as gcc 13.3.0, clang 19.1.1 and clang 18.1.3 refuse it. Asserting
// the old code on the old fixture would now be asserting that the earlier bug is
// still present.
//   The fixture is therefore the EMPTY initializer — the one form C23 6.7.10p4
// permits, which really is a legal VLA and really does reach this validator, so
// the constexpr constraint is still the thing under test. The declarator-scan
// arm is separately exercised below.
// RED-ON-DISABLE: delete BOTH the `isVlaArray` test and the declarator scan and
// this test accepts.
TEST(SemanticAnalyzerC, ConstexprVariablyModifiedAggregateFailsLoud) {
    auto model = analyzeShipped("c", {
        "int main(int argc, char **argv) {\n"
        "    constexpr int a[argc] = {};\n"
        "    return a[0];\n"
        "}\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConstexprUnsupportedType), 1u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_VlaInitializerNotEmpty), 0u)
        << "the EMPTY initializer is legal for a VLA — what is refused here is "
           "the `constexpr`, not the initializer";
}

// ...and the NON-empty sibling, which is now refused one tier earlier. Kept as
// its own pin so a future change that re-admitted the dropped bound would have to
// break a test that names the exact construct, rather than quietly re-greening
// the one above.
TEST(SemanticAnalyzerC, ConstexprVariablyModifiedWithNonEmptyInitIsRefusedEarlier) {
    auto model = analyzeShipped("c", {
        "int main(int argc, char **argv) {\n"
        "    constexpr int a[argc] = {1, 2, 3};\n"
        "    return a[0];\n"
        "}\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_VlaInitializerNotEmpty), 1u)
        << "all three references refuse this for the INITIALIZER, before the "
           "`constexpr` is even considered";
    EXPECT_TRUE(model.hasErrors());
}

// isConstexpr IMPLIES isConst end-to-end: assigning to a constexpr object is
// rejected by the EXISTING const-violation machinery (code-audit MEDIUM-3 pin —
// the flag pin alone doesn't prove the assignment path fires).
TEST(SemanticAnalyzerC, ConstexprAssignmentRejected) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ConstexprExactRepresentabilityCurrentlyUnenforced) {
    auto fracModel = analyzeShipped("c", {
        "int main(void) { constexpr int x = 1.5; return x; }\n",
    });
    EXPECT_FALSE(fracModel.hasErrors())
        << "documents the OPEN 6.7.1p10 boundary — update when the deferral closes";
    auto negModel = analyzeShipped("c", {
        "int main(void) { constexpr unsigned int u = -1; return 0; }\n",
    });
    EXPECT_FALSE(negModel.hasErrors())
        << "documents the OPEN 6.7.1p10 boundary — update when the deferral closes";
}

// ── FC17.5 (D-CSUBSET-EMPTY-INITIALIZER + D-CSUBSET-FUNC-PREDEFINED-IDENTIFIER):
//    C23 {} empty/scalar brace-init × constexpr, and the C99
//    6.4.2.2 `__func__` predefined identifier ────────────────────────────────

// F3 (C23 6.7.10p11 × 6.7.1): an EMPTY brace initializer zero-initializes —
// zero is a valid compile-time value for every scalar constexpr object,
// including the pointer arm (zero = the null pointer constant). Without the
// F3 empty-brace arm, `constExprValue` cannot fold `{}` (it is not an
// expression) and both declarations would false-fire
// S_ConstexprNonConstantInitializer.
TEST(SemanticAnalyzerC, ConstexprEmptyBraceInitializerValid) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ConstexprSingleBraceInitializerValid) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FuncNameAssignmentIsConstViolation) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FuncNameCompoundAssignIsConstViolation) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ParamNamedFuncNameRedeclares) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, FuncNameOutsideFunctionIsUndeclared) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, MaybeUnusedSuppressesUnusedWarning) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, GnuUnusedSpellingSuppresses) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, MaybeUnusedMultiDeclaratorAppliesAll) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, DeprecatedWarnsAtEachCallSite) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, DeprecatedMessageIncluded) {
    auto model = analyzeShipped("c", {
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

// ★ THE TF-C62 REGRESSION PIN: the GNU `__attribute__((deprecated("m")))` form
// must carry its message exactly like the C23 form. TF-C62 moved the GNU
// argument out of a direct `stringLiteralExpr` child down into an `attrArgs`
// subtree (`attrArgs > attrArgList > attrArgItem > attrArgAtom >
// stringLiteralExpr` after TF-C72); the clause extractor still read the FIRST
// Internal child, handing the decode chokepoint a node with no body children →
// it returned "" (NOT nullopt) and the message was SILENTLY dropped. MEASURED at
// the defect: `warning[S003D] got f` — the message simply gone. The bug survived
// because the GNU form was pinned only by diagnostic COUNTS; this asserts TEXT.
TEST(SemanticAnalyzerC, GnuDeprecatedMessageIncluded) {
    auto model = analyzeShipped("c", {
        "__attribute__((deprecated(\"use g\"))) int f(void);\n"
        "int f(void) { return 1; }\n"
        "int main(void) { return f(); }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u);
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_DeprecatedSymbolUsed)
            EXPECT_EQ(diag.actual, "f: use g")
                << "the GNU form's message must survive the attrArgs nesting";
    }
}

// The GNU and C23 spellings are the SAME attribute: identical source message ⇒
// identical diagnostic text. The parity assertion is the regression wall for the
// C23 path (which worked throughout the defect) AND for the GNU path together —
// a future reshape of EITHER argument grammar breaks this before it ships.
TEST(SemanticAnalyzerC, DeprecatedMessageSameForGnuAndC23) {
    auto model = analyzeShipped("c", {
        "__attribute__((deprecated(\"use g\"))) int f(void);\n"
        "[[deprecated(\"use g\")]] int h(void);\n"
        "int f(void) { return 1; }\n"
        "int h(void) { return 2; }\n"
        "int main(void) { return f() + h(); }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 2u);
    std::string gnuText;    // the GNU-declared f
    std::string stdText;    // the C23-declared h
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code != DiagnosticCode::S_DeprecatedSymbolUsed) continue;
        if (diag.actual.rfind("f", 0) == 0) gnuText = diag.actual;
        if (diag.actual.rfind("h", 0) == 0) stdText = diag.actual;
    }
    EXPECT_EQ(gnuText, "f: use g") << "GNU form";
    EXPECT_EQ(stdText, "h: use g") << "C23 form";
}

// C 5.1.1.2 phase 6: `deprecated("use " "g")` is ONE argument (adjacent literals
// concatenate), not two — the subtree search STOPS at the `stringLiteralExpr`
// node and hands the whole run to the shared `decodeAdjacentStringBodies`
// chokepoint, so the pieces JOIN rather than reading as an ambiguous pair.
TEST(SemanticAnalyzerC, GnuDeprecatedAdjacentConcatMessage) {
    auto model = analyzeShipped("c", {
        "__attribute__((deprecated(\"use \" \"g\"))) int f(void);\n"
        "int f(void) { return 1; }\n"
        "int main(void) { return f(); }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "an adjacent-concatenated message is ONE argument, never ambiguous";
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u);
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_DeprecatedSymbolUsed)
            EXPECT_EQ(diag.actual, "f: use g");
    }
}

// `__attribute__((deprecated))` with NO argument is legal and has NO message —
// the `.actual` is the BARE symbol name, with no `: ` separator and no crash.
// A REGRESSION WALL (green before and after the attrArgs-descent fix), not a
// red-on-disable pin: the extractor's nullopt-vs-"" distinction is real at the
// EXTRACTION boundary, but MEASURED it cannot be observed here — making "found
// nothing" yield "" instead of nullopt leaves this test GREEN, because
// `AttributeSemanticsFacts`/`SymbolRecord` store the message as a `std::string`
// merged first-NON-EMPTY-wins and the render site omits the `: ` separator for an
// empty message. So `deprecated("")` and no-argument are indistinguishable
// DOWNSTREAM today; carrying the distinction further is a separate change.
TEST(SemanticAnalyzerC, GnuDeprecatedNoArgumentHasNoMessage) {
    auto model = analyzeShipped("c", {
        "__attribute__((deprecated)) int f(void);\n"
        "int f(void) { return 1; }\n"
        "int main(void) { return f(); }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u);
    for (auto const& diag : model.diagnostics().all())
        if (diag.code == DiagnosticCode::S_DeprecatedSymbolUsed)
            EXPECT_EQ(diag.actual, "f")
                << "no argument ⇒ no message, NOT an empty one";
}

// FAIL LOUD: a MODELLED attribute whose clause carries TWO string arguments has
// no unambiguous message — picking the first would be a fresh silent drop of the
// second (the `linkageFrom` adjacent-concat fail-loud posture). S_UnknownTypeAttribute
// at Error severity, and the deprecation warning stays MESSAGE-LESS: the engine
// never guesses which string was meant.
// RED-ON-DISABLE: drop the ambiguity branch and take the first string → no error
// and `.actual` becomes "x: a".
TEST(SemanticAnalyzerC, GnuDeprecatedTwoStringArgumentsFailLoud) {
    auto model = analyzeShipped("c", {
        "int main(void) {\n"
        "    __attribute__((deprecated(\"a\", \"b\"))) int x = 0;\n"
        "    return x;\n"
        "}\n",
    });
    EXPECT_TRUE(model.hasErrors())
        << "an ambiguous attribute message must never compile silently";
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 1u);
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_UnknownTypeAttribute) {
            EXPECT_EQ(diag.severity, DiagnosticSeverity::Error);
            EXPECT_EQ(diag.actual,
                      "attribute 'deprecated' carries more than one string argument");
        }
        if (diag.code == DiagnosticCode::S_DeprecatedSymbolUsed)
            EXPECT_EQ(diag.actual, "x")
                << "the ambiguous message must be DROPPED, never guessed";
    }
}

// FAIL LOUD, second door: the shared decode chokepoint returns nullopt on a
// MALFORMED escape (C 5.1.1.2 phase 5). Unreported that is the SAME silent
// message drop as the attrArgs-nesting defect — MEASURED before this gate:
// `deprecated("bad\uZZZZ")` warned a bare `x`, the message quietly gone.
// RED-ON-DISABLE: drop the `!out.message.has_value()` fail-loud → no error and
// the deprecation warning silently loses its message again.
TEST(SemanticAnalyzerC, GnuDeprecatedMalformedEscapeFailsLoud) {
    auto model = analyzeShipped("c", {
        "int main(void) {\n"
        "    __attribute__((deprecated(\"bad\\uZZZZ\"))) int x = 0;\n"
        "    return x;\n"
        "}\n",
    });
    EXPECT_TRUE(model.hasErrors())
        << "an undecodable attribute message must never be dropped silently";
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 1u);
    for (auto const& diag : model.diagnostics().all())
        if (diag.code == DiagnosticCode::S_UnknownTypeAttribute)
            EXPECT_EQ(diag.actual,
                      "attribute 'deprecated' has a malformed escape in its "
                      "string argument");
}

// A COMMA-separated sibling clause (`attrClauseTail`) is a DIFFERENT attribute —
// its string is not this clause's message. The subtree search skips wholesale any
// nested node carrying its OWN name identifier, which is precisely the "the name
// is a direct identifier child" clause model applied recursively.
// RED-ON-DISABLE: drop that skip → `deprecated` adopts the sibling's "hidden"
// and `.actual` becomes "x: hidden" (a silently WRONG message).
TEST(SemanticAnalyzerC, GnuSiblingClauseStringIsNotThisClausesMessage) {
    auto model = analyzeShipped("c", {
        "int main(void) {\n"
        "    __attribute__((deprecated, __visibility__(\"hidden\"))) int x = 0;\n"
        "    return x;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u);
    for (auto const& diag : model.diagnostics().all())
        if (diag.code == DiagnosticCode::S_DeprecatedSymbolUsed)
            EXPECT_EQ(diag.actual, "x")
                << "a LATER clause's string must never become this one's message";
}

// The ambiguity gate is scoped to attributes the language MODELS. A real SDK-header
// shape the effect table does not model — `__availability__(macos, message="a",
// replacement="b")`, TF-C72's motivating case — carries two strings legitimately and
// must stay exactly as inert as before: parsed, ignored, NEVER newly rejected.
// RED-ON-DISABLE: drop the `row != nullptr` gate → this errors S_UnknownTypeAttribute
// and every macOS SDK header using the availability form stops compiling.
TEST(SemanticAnalyzerC, UnmodelledGnuMultiStringAttributeStaysInert) {
    auto model = analyzeShipped("c", {
        "int main(void) {\n"
        "    __attribute__((__availability__(macos, message=\"a\","
        " replacement=\"b\"))) int x = 0;\n"
        "    return x;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "an unmodelled multi-string GNU attribute must not be rejected";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u);
}

// A `[[deprecated]]` PROTOTYPE + an unflagged definition: the flag OR-merges
// into the surviving definition (the isNoreturn mergedFnDecls precedent), so a
// call — which resolves to the survivor — still warns. RED-ON-DISABLE for the
// merge: drop the OR-merge block → detection marked only the absorbed proto,
// the survivor stays unflagged, the count drops to 0.
TEST(SemanticAnalyzerC, DeprecatedProtoOrMergesIntoDefinition) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, DeprecatedObjectUseWarns) {
    auto model = analyzeShipped("c", {
        "[[deprecated]] int legacy_flag;\n"
        "int main(void) { return legacy_flag; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u)
        << "a deprecated object's use site must warn like a function's";
}

// ── P33 (D-CSUBSET-ATTRIBUTE-DEPRECATED-TYPES, C23 6.7.13.3) ────────────────
//
// A deprecated TYPE — a struct/union TAG or a typedef name — warns at every USE.
// ★ THE TAG'S ATTRIBUTE POSITION IS AFTER THE KEYWORD, and that is not a stylistic
// choice: ✔MEASURED, gcc 13.3.0 calls the PREFIX spelling `[[deprecated]] struct S
// { … };` an "empty declaration" and DISCARDS the attribute, while clang 19.1.1
// and clang 18.1.3 REFUSE it outright ("misplaced attributes"). Both references
// honor `struct [[deprecated]] S { … };` and warn at every use of the tag, so that
// is the spelling this pins.
// RED-ON-DISABLE: drop the `srec.isDeprecated` write at the composite-composition
// site and every tag arm here reports 0 warnings.
TEST(SemanticAnalyzerC, DeprecatedCompositeTagUseWarns) {
    auto model = analyzeShipped("c", {
        "struct [[deprecated]] Legacy { int x; };\n"
        "struct Legacy gv;\n"
        "int main(void) { struct Legacy v; v.x = 1; return gv.x + v.x; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 2u)
        << "one warning per tag USE site — the file-scope object and the local";
}

// The GNU spelling of the same position. ⚠ At HEAD this was a hard ERROR
// (S_UnknownTypeAttribute): `deprecated` matched neither `packed` nor an `Align`
// row in the composite attribute scan, so it fell to the strict-unknown arm — a
// REFUSAL of a construct gcc accepts. The verb-driven `warnOnUse` arm makes the
// two spellings agree.
// RED-ON-DISABLE: remove the `WarnOnUse` arm from `scanCompositePacked` and this
// reds with 1 S_UnknownTypeAttribute and 0 warnings.
TEST(SemanticAnalyzerC, GnuDeprecatedCompositeTagUseWarns) {
    auto model = analyzeShipped("c", {
        "struct __attribute__((deprecated)) Legacy { int x; };\n"
        "struct Legacy gv;\n"
        "int main(void) { return gv.x; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u)
        << "a modelled warnOnUse name is not an unknown composite attribute";
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u);
}

TEST(SemanticAnalyzerC, DeprecatedCompositeTagMessageIncluded) {
    auto model = analyzeShipped("c", {
        "struct [[deprecated(\"use Legacy2\")]] Legacy { int x; };\n"
        "struct Legacy gv;\n"
        "int main(void) { return gv.x; }\n",
    });
    ASSERT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u);
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_DeprecatedSymbolUsed) {
            EXPECT_EQ(diag.severity, DiagnosticSeverity::Warning);
            EXPECT_EQ(diag.actual, "Legacy: use Legacy2");
        }
    }
}

// A deprecated TYPEDEF NAME. Unlike a tag, it has no Pass-2 reference chokepoint —
// in type position it is a bare identifier token under a type-specifier run — so
// the warning comes from the type resolver's alias arm.
// RED-ON-DISABLE: delete that arm and every count here drops to 0.
TEST(SemanticAnalyzerC, DeprecatedTypedefUseWarns) {
    auto declModel = analyzeShipped("c", {
        "typedef int oldint [[deprecated(\"use int32\")]];\n"
        "oldint gi;\n"
        "int main(void) { return gi; }\n",
    });
    EXPECT_FALSE(declModel.hasErrors());
    ASSERT_EQ(countCode(declModel.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u);
    for (auto const& diag : declModel.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_DeprecatedSymbolUsed)
            EXPECT_EQ(diag.actual, "oldint: use int32");
    }
    auto gnuModel = analyzeShipped("c", {
        "typedef int oldint __attribute__((deprecated));\n"
        "oldint gi;\n"
        "int main(void) { return gi; }\n",
    });
    EXPECT_EQ(countCode(gnuModel.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u)
        << "the GNU trailing spelling carries the same fact";
}

// ★★ THE POSITIONS BOTH REFERENCES WARN AT, and the two that exercise the
// EMIT-GATE. A cast type-name and a `_Generic` association are resolved once
// SPECULATIVELY under a reporter-rollback window and once LOUDLY outside it; a
// warning emitted on the rolled-back resolve would vanish, and one emitted on
// both without the per-node latch would double. Exactly one each is the proof
// that the gate and the latch are both doing their job.
// ✔MEASURED gcc 13.3.0 and clang 19.1.1: one warning at each of these positions.
TEST(SemanticAnalyzerC, DeprecatedTypedefWarnsOncePerUsePosition) {
    auto model = analyzeShipped("c", {
        "typedef int oldint [[deprecated]];\n"
        "int f(oldint a);\n"
        "int f(oldint a) { return a; }\n"
        "int main(void) {\n"
        "    int x = 0;\n"
        "    int c = (oldint)x;\n"
        "    int g = _Generic(x, oldint: 0, default: 1);\n"
        "    return f(0) + c + g + (int)sizeof(oldint) - 4;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 5u)
        << "proto param, definition param, cast, _Generic association, sizeof — "
           "each exactly once";
}

// ★★ THE LATCH'S OWN RED-ON-DISABLE, and it took a second attempt to build one
// that is not vacuous. Each PARAMETER type-name node reaches the alias arm TWICE
// (✔MEASURED by instrumenting the arm with a per-entry sequence number, which
// defeats the reporter's dedup key). With ONE parameter the two entries are
// ADJACENT and the reporter's default 4-entry `dedupWindow` collapses them, so
// removing the latch changes nothing observable — the first mutant came back
// GREEN. FIVE parameters push the repeats 5 apart, past the window: without
// `deprecatedTypeUseWarned` this source reports 10. gcc 13.3.0 and clang 19.1.1
// each report 5 (MEASURED).
TEST(SemanticAnalyzerC, DeprecatedTypedefParamsWarnOncePerParameterNotTwice) {
    auto model = analyzeShipped("c", {
        "typedef int oldint [[deprecated]];\n"
        "int f(oldint a, oldint b, oldint c, oldint d, oldint e);\n"
        "int main(void) { return f(0, 0, 0, 0, 0); }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 5u)
        << "one warning per parameter — the per-node latch, not the dedup window";
}

// One type-name node shared by three declarators is ONE use site, so ONE warning
// — the per-node latch, not the reporter's dedup window, is what makes this true.
TEST(SemanticAnalyzerC, DeprecatedTypedefMultiDeclaratorWarnsOnce) {
    auto model = analyzeShipped("c", {
        "typedef int oldint [[deprecated]];\n"
        "oldint a, b, c;\n"
        "int main(void) { return a + b + c; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u);
}

// The NEGATIVE wall: an undeprecated type must never warn, and a composite
// attribute this engine does NOT model in the `warnOnUse` verb keeps its
// pre-existing loud reject (the typo protection `scanCompositePacked` exists for).
TEST(SemanticAnalyzerC, UndeprecatedTypesDoNotWarnAndUnknownStrictStillRefuses) {
    auto cleanModel = analyzeShipped("c", {
        "struct Plain { int x; };\n"
        "typedef int myint;\n"
        "struct Plain gp;\n"
        "myint gi;\n"
        "int main(void) { return gp.x + gi; }\n",
    });
    EXPECT_EQ(countCode(cleanModel.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 0u);
    auto typoModel = analyzeShipped("c", {
        "struct __attribute__((pakced)) S { char c; int x; };\n"
        "int main(void) { return (int)sizeof(struct S); }\n",
    });
    EXPECT_EQ(countCode(typoModel.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 1u)
        << "an unmodelled GNU composite attribute still fails loud";
}

// C23 6.7.13.2: a `[[nodiscard]]` call whose result is DISCARDED — the call is
// the entire expression of an expression statement — warns (Warning severity,
// naming the callee).
TEST(SemanticAnalyzerC, NodiscardDiscardedWarns) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, NodiscardVoidCastSuppresses) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, NodiscardUsedInExpressionNoWarn) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, NodiscardMessageIncluded) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, GnuWarnUnusedResultWarnsAndMerges) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, UnknownStdAttrWarnsSuppressibly) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, KnownC23NoOpAttributesDontWarn) {
    auto model = analyzeShipped("c", {
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
// examples/c/switch_fallthrough_attribute corpus entry.
TEST(SemanticAnalyzerC, FallthroughStatementParsesInSwitch) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, GnuFallthroughSpellingParses) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, UnknownBareStatementAttrWarnsSuppressibly) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AutoInfersIntPass15Visible) {
    auto model = analyzeShipped("c", {
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

// D-MIR-ELEMENT-CONST-ARRAY-GLOBAL-CLASSIFICATION (the PRIMARY red-on-disable
// pin): a file-scope ARRAY whose ELEMENTS are const pointers — the direct-
// declarator spelling `int (*const ops[N])(int)` — is a const object (C 6.7.3p9
// so-qualifies the element; gcc/clang park such a fn-ptr table in relocated-read-
// only `.data.rel.ro`). Its `SymbolRecord.isConst` must be TRUE so it threads
// MutabilityAttr → MirGlobal.isConst → the asm relocBearingGlobalSection
// chokepoint routes it to RelRoConst, not writable `.data`. The const pointer
// layer hides inside the parenthesized group, so this REDS if declaratorObject-
// IsConst stops descending the group (the object then falls to the head scan,
// which sees no `const` in `int` → isConst=false → mis-routed to `.data`). This
// is the shape of sqlite3.c's `static int (*const sqlite3BuiltinExtensions[])
// (sqlite3*) = {…};`. A NON-const sibling and the typedef'd spelling anchor the
// two ends (must-not-over-classify / must-stay-correct).
TEST(SemanticAnalyzerC, ElementConstFnPtrArrayGlobalIsConst) {
    auto model = analyzeShipped("c", {
        "int a(int x){ return x + 10; }\n"
        "int b(int x){ return x + 20; }\n"
        "int (*const ops[2])(int) = { a, b };\n"        // array of CONST fn-ptrs
        "int (*muts[2])(int) = { a, b };\n"             // array of MUTABLE fn-ptrs
        "typedef int (*op)(int);\n"
        "const op tab[2] = { a, b };\n"                 // typedef'd spelling
        "static int (*const exts[])(int) = { a };\n",   // sqlite's inferred static
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* ops = findSymbolNamed(model, "ops");
    ASSERT_NE(ops, nullptr);
    EXPECT_TRUE(ops->isConst)
        << "int (*const ops[2])(int): the array's elements are const pointers "
           "→ a const object → RelRoConst; the group-hidden `* const` layer must "
           "be found by descending the parenthesized group";
    auto const* muts = findSymbolNamed(model, "muts");
    ASSERT_NE(muts, nullptr);
    EXPECT_FALSE(muts->isConst)
        << "int (*muts[2])(int): mutable fn-ptr elements → NOT const (the fix "
           "keys on the `* const` marker, never merely on the grouping)";
    auto const* tab = findSymbolNamed(model, "tab");
    ASSERT_NE(tab, nullptr);
    EXPECT_TRUE(tab->isConst)
        << "const op tab[2] (typedef spelling) stays const — the head-const path "
           "is untouched by the group descent";
    auto const* exts = findSymbolNamed(model, "exts");
    ASSERT_NE(exts, nullptr);
    EXPECT_TRUE(exts->isConst)
        << "static int (*const exts[])(int) — sqlite's inferred-size const fn-ptr "
           "table — is a const object too";
}

// ★C1 — the auto-presence gate: all four specifier-led C89 implicit-int
// shapes PARSE into the headless rule and MUST stay errors
// (S_AutoInferenceInvalid, one per declaration). RED-ON-DISABLE: drop the
// row's requiredSpecifierToken (or the arm's gate) and each silently
// becomes an initializer-typed declaration.
TEST(SemanticAnalyzerC, AutoPresenceGateKeepsImplicitIntErrors) {
    char const* const forms[] = {
        "int main(void) { static x = 5; return x; }\n",
        "int main(void) { register y = 2; return y; }\n",
        "int main(void) { alignas(4) z = 9; return z; }\n",
        "int main(void) { [[maybe_unused]] w = 3; return w; }\n",
    };
    for (auto const* src : forms) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_AutoInferenceInvalid), 1u)
            << "C89 implicit-int must stay an error for: " << src;
    }
}

// ★C3 decay — a string-literal initializer infers char* (NOT Array<Char,4>):
// the un-decayed array would give sizeof(s)==4 and a wrong-typed object.
TEST(SemanticAnalyzerC, AutoStringLiteralDecaysToCharPointer) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AutoArrayVariableDecaysToElementPointer) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AutoFunctionNameDecaysToFunctionPointer) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AutoMultiDeclaratorRejected) {
    auto model = analyzeShipped("c", {
        "int main(void) { auto a = 1, b = 2; return a + b; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AutoRequiresSingleDeclarator), 1u);
}

// C23 6.7.9p2 — a PLAIN IDENTIFIER declarator. `auto *p = 0;` (a derived
// declarator — D-CSUBSET-AUTO-DERIVED-DECLARATOR) and the diagnostic-SHAPE
// pin `auto f(void);` (parses into the headless rule; must be THIS code,
// never a silent prototype) both reject S_AutoRequiresPlainIdentifier.
TEST(SemanticAnalyzerC, AutoDerivedDeclaratorRejected) {
    auto ptrModel = analyzeShipped("c", {
        "int main(void) { auto *p = 0; return 0; }\n",
    });
    EXPECT_EQ(countCode(ptrModel.diagnostics(),
                        DiagnosticCode::S_AutoRequiresPlainIdentifier), 1u);
    auto fnModel = analyzeShipped("c", {
        "int main(void) { auto f(void); return 0; }\n",
    });
    EXPECT_EQ(countCode(fnModel.diagnostics(),
                        DiagnosticCode::S_AutoRequiresPlainIdentifier), 1u);
}

// C23 6.7.9p2 — an initializer is REQUIRED. `auto T;` is the second
// diagnostic-SHAPE pin (the one dual-parse shape `<specifiers> Ident ;`
// must surface as THIS inference-tier code).
TEST(SemanticAnalyzerC, AutoMissingInitializerRejected) {
    auto model = analyzeShipped("c", {
        "int main(void) { auto T; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AutoRequiresInitializer), 1u);
}

// ★C3 rejects — void call / bare nullptr / self-reference each fail loud
// S_AutoInferenceInvalid (RED-ON-DISABLE: without the arm's rejects, Pass
// 2's initializer backfill silently adopts Void / NullptrT / nothing).
TEST(SemanticAnalyzerC, AutoUninferableInitializersRejected) {
    auto voidModel = analyzeShipped("c", {
        "void vf(void) { }\n"
        "int main(void) { auto v = vf(); return 0; }\n",
    });
    EXPECT_EQ(countCode(voidModel.diagnostics(),
                        DiagnosticCode::S_AutoInferenceInvalid), 1u)
        << "auto from a void call must reject";
    auto nullModel = analyzeShipped("c", {
        "int main(void) { auto p = nullptr; return 0; }\n",
    });
    EXPECT_EQ(countCode(nullModel.diagnostics(),
                        DiagnosticCode::S_AutoInferenceInvalid), 1u)
        << "auto from bare nullptr must reject (nullptr_t not declarable)";
    auto selfModel = analyzeShipped("c", {
        "int main(void) { auto x = x; return 0; }\n",
    });
    EXPECT_EQ(countCode(selfModel.diagnostics(),
                        DiagnosticCode::S_AutoInferenceInvalid), 1u)
        << "self-referential initializer must reject loud";
}

// C23 6.7.9p2 via 6.7.10p12 — the braced SINGLE form infers; the empty and
// multi-element forms reject via the shared scalar-brace constraint code.
TEST(SemanticAnalyzerC, AutoBracedSingleInfersAndMalformedRejects) {
    auto okModel = analyzeShipped("c", {
        "int main(void) { auto x = {5}; return x - 5; }\n",
    });
    EXPECT_FALSE(okModel.hasErrors());
    auto const* x = findSymbolNamed(okModel, "x");
    ASSERT_NE(x, nullptr);
    ASSERT_TRUE(x->type.valid());
    EXPECT_EQ(okModel.lattice().interner().kind(x->type), TypeKind::I32);
    auto multiModel = analyzeShipped("c", {
        "int main(void) { auto y = {1, 2}; return 0; }\n",
    });
    EXPECT_EQ(countCode(multiModel.diagnostics(),
                        DiagnosticCode::S_InvalidScalarInitializer), 1u);
    auto emptyModel = analyzeShipped("c", {
        "int main(void) { auto z = {}; return 0; }\n",
    });
    EXPECT_EQ(countCode(emptyModel.diagnostics(),
                        DiagnosticCode::S_InvalidScalarInitializer), 1u)
        << "`auto z = {};` has no expression to infer from";
}

// The C89 REGRESSION pin: `auto int x;` (auto as a plain storage-class with
// a real type head) must keep parsing via varDecl on rollback — the
// inference rule fast-fails on the `int` and the committed path types x int.
TEST(SemanticAnalyzerC, AutoC89StorageClassFormUnchanged) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AutoTopLevelVolatileStripped) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AutoConstexprComposes) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AutoHugeStaticInitParsesViaCommittedReplay) {
    std::string src = "int main(void) {\n    static const int big[] = {";
    for (int i = 0; i < 2200; ++i) {          // ~4400 tokens inside the braces
        if (i > 0) src += ',';
        src += std::to_string(i % 97);
    }
    src += "};\n    return big[3];\n}\n";
    auto model = analyzeShipped("c", {src});
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
TEST(SemanticAnalyzerC, AutoForInitInfersAndStaticStaysGated) {
    auto okModel = analyzeShipped("c", {
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
    auto gatedModel = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, AutoFileScopeAndQualifiedStayLoudParseErrors) {
    char const* const rejects[] = {
        "auto g = 42;\nint main(void) { return g; }\n",
        "int main(void) { const auto x = 5; return x; }\n",
        "int main(void) { auto const x = 5; return x; }\n",
    };
    for (auto const* src : rejects) {
        auto cu = buildShippedUnit("c", {std::string{src}});
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
// comparison (I32 — a comparison RESULT type is C's `int`, C 6.5.8p6, sourced
// config-drivenly by subtreeType; D-CSUBSET-SIZEOF-COMPARISON-INT-TYPE), a char
// variable (Char, not the promoted int), and an unsuffixed float literal (F64
// per C 6.4.4.2). All in ONE unit so the block also witnesses the inferred
// objects USED together (member access through the inferred struct, arithmetic
// across the rest).
TEST(SemanticAnalyzerC, AutoInfersExactKindsAcrossValueClasses) {
    auto model = analyzeShipped("c", {
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
    EXPECT_EQ(kindOf("b"), TypeKind::I32)
        << "a comparison infers int (C 6.5.8p6: the RESULT type of a relational "
           "operator is int, sourced config-drivenly — "
           "D-CSUBSET-SIZEOF-COMPARISON-INT-TYPE; the i1/Bool SSA carrier is the "
           "separate machine-tier concern)";
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
TEST(SemanticAnalyzerC, ThreadLocalAcceptsAndMarksSymbols) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ThreadLocalDoesNotClobberStaticBinding) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ThreadLocalOnFunctionFailsLoud) {
    auto proto = analyzeShipped("c", {
        "thread_local int f(void);\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(proto.diagnostics(),
                        DiagnosticCode::S_ThreadLocalOnFunction), 1u)
        << "a thread_local prototype is a 6.7.1p4 constraint violation";
    auto def = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ThreadLocalBlockScopeRequiresStaticOrExtern) {
    auto plain = analyzeShipped("c", {
        "int main(void) { thread_local int x = 1; return x; }\n",
    });
    EXPECT_EQ(countCode(plain.diagnostics(),
                        DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern),
              1u);
    auto forInit = analyzeShipped("c", {
        "int main(void) {\n"
        "    for (thread_local int i = 0; i < 2; i = i + 1) {}\n"
        "    return 0;\n"
        "}\n",
    });
    EXPECT_EQ(countCode(forInit.diagnostics(),
                        DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern),
              1u)
        << "a for-init thread_local can never satisfy 6.7.1p3 (gatedMarkers)";
    auto autoForm = analyzeShipped("c", {
        "int main(void) { thread_local auto x = 5; return x; }\n",
    });
    EXPECT_EQ(countCode(autoForm.diagnostics(),
                        DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern),
              1u)
        << "the C23 auto-inferred block decl is caught by the same check";
    // The LEGAL counterpart pins the check polarity: static satisfies p3.
    auto legal = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ThreadLocalRedeclarationMismatchBothDirections) {
    auto gained = analyzeShipped("c", {
        "extern int g;\n"
        "thread_local int g = 5;\n"
        "int main(void) { return g; }\n",
    });
    EXPECT_EQ(countCode(gained.diagnostics(),
                        DiagnosticCode::S_ThreadLocalRedeclarationMismatch),
              1u)
        << "plain extern then thread_local definition must mismatch";
    auto lost = analyzeShipped("c", {
        "extern thread_local int g;\n"
        "int g = 5;\n"
        "int main(void) { return g; }\n",
    });
    EXPECT_EQ(countCode(lost.diagnostics(),
                        DiagnosticCode::S_ThreadLocalRedeclarationMismatch),
              1u)
        << "extern thread_local then plain definition must mismatch too";
    // The MATCHED pair is legal — pins the check polarity.
    auto matched = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ThreadLocalInvalidCombinationsFailLoud) {
    auto cxFirst = analyzeShipped("c", {
        "constexpr thread_local int c = 5;\n"
        "int main(void) { return c; }\n",
    });
    EXPECT_EQ(countCode(cxFirst.diagnostics(),
                        DiagnosticCode::S_ThreadLocalInvalidCombination), 1u);
    auto cxSecond = analyzeShipped("c", {
        "thread_local constexpr int c = 5;\n"
        "int main(void) { return c; }\n",
    });
    EXPECT_EQ(countCode(cxSecond.diagnostics(),
                        DiagnosticCode::S_ThreadLocalInvalidCombination), 1u)
        << "specifier order must not matter (the mark-based check)";
    auto reg = analyzeShipped("c", {
        "int main(void) { register thread_local int r = 1; return r; }\n",
    });
    EXPECT_EQ(countCode(reg.diagnostics(),
                        DiagnosticCode::S_ThreadLocalInvalidCombination), 1u)
        << "register may not pair with thread_local (6.7.1p2)";
}

// VLA C4a-local (D-CSUBSET-VLA): a pointer-to-VLA assignability compare stays EXACT — a
// FIXED-pointee `int (*p)[5]` initialized from a VLA object `int b[2][n]` (rows int[n])
// is a MISMATCH (`Ptr<int[5]>` vs `array(vlaArray(int),2)`; int[5] != int[n]) and must
// REJECT with S_TypeMismatch, never silently decay-accept. ★ This was written as the
// forward-guard for the then-deferred init form (D-CSUBSET-VLA-PTR-INIT-FORM-TYPING):
// whatever made `= b` work must not weaken this exact-row compare. That row CLOSED in
// P34 and the guard HELD — `int (*p)[n] = b;` is accepted while this stays a reject, so
// the compare narrowed on the POINTEE's row length rather than being loosened. RED-ON-DISABLE: broaden the array-to-pointer decay
// branch to ignore the element type → this stops firing.
TEST(SemanticAnalyzerC, PtrToVlaFixedPointeeFromVlaObjectRejects) {
    auto model = analyzeShipped("c", {
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

// VLA C4a-local (D-CSUBSET-VLA): the ptr-to-VLA init-form work
// (D-CSUBSET-VLA-PTR-INIT-FORM-TYPING, CLOSED in P34) must NEVER regress ordinary
// aggregate brace-init —
// `int a[3]={1,2,3}` / nested / a scalar init all stay clean (no false S_TypeMismatch
// from a subtreeType descent into a braceInitList). The CRITICAL-1 control that keeps the
// eventual init-form fix guarded. RED-ON-DISABLE: an unguarded subtreeType override on the
// init-derivation path would descend a brace list to a member literal → this reds.
TEST(SemanticAnalyzerC, LocalAggregateBraceInitStaysCleanNoFalseTypeMismatch) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, VlaTypedefObjectAcceptsAndRecordsOrigin) {
    auto model = analyzeShipped("c", {
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
// type-checks (zero S_TypeMismatch). The runtime witness is examples/c/
// c99_vla_ptr_param (a genuine VLA-object caller is a NON-leaf VLA function — a C1b
// deferral — so the runnable example casts a fixed buffer; THIS pin covers the VLA-arg
// decay type-check that the example cannot exercise at runtime). RED-ON-DISABLE: revert the
// paramDecay threading and the pointee never becomes a VLA row → the arg fails the exact
// decay compare → S_TypeMismatch reappears.
TEST(SemanticAnalyzerC, ParamPtrToVlaAcceptsAndVlaArgDecays) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ParamPtrToFixedArrayStillAccepts) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ParamAdjustedArrayOfVlaAccepts) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, PlainVlaArrayParamStillDecaysToPointer) {
    auto model = analyzeShipped("c", {
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
// paramAdjustments), so the FAM path is byte-identical. RED-ON-DISABLE: a broad fix that
// routed struct fields through the paramDecay VLA branch would build a vlaArray instead of
// an incompleteArray and this diagnostic vanishes.
TEST(SemanticAnalyzerC, StructFieldVlaSoleMemberStillFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ParamPtrToVlaFixedArgRejects) {
    auto model = analyzeShipped("c", {
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
// parse) fails. Runtime witness: examples/c/c99_array_param_static.
TEST(SemanticAnalyzerC, ArrayParamStaticDecaysToPointer) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ArrayParamDecorationsAllDecayClean) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ArrayParamStarFormDecaysCleanNonParamFailsLoud) {
    // PARAMETER `int a[*]` — decays to a bare pointer, compiles clean.
    auto param = analyzeShipped("c", {
        "int f(int a[*]) { return a[0]; }\n"
        "int main(void) { int x[1]; x[0] = 7; return f(x); }\n",
    });
    EXPECT_FALSE(param.hasErrors())
        << "a PARAMETER `int a[*]` must decay to a bare pointer + compile clean";
    EXPECT_EQ(countCode(param.diagnostics(),
                        DiagnosticCode::S_ArrayParamQualifierNonParameter), 0u)
        << "a `[*]` on a PARAMETER must NOT trip the non-parameter gate";

    // NON-PARAMETER `int a[*];` (a local) — a constraint violation → 0xE054.
    auto local = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ArrayStaticOnLocalFailsLoud) {
    auto model = analyzeShipped("c", {
        "int main(void) { int a[static 3]; return a[0]; }\n",
    });
    EXPECT_TRUE(hasCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayParamQualifierNonParameter))
        << "a LOCAL `int a[static 3]` is a non-parameter constraint violation -> 0xE054";
}

// The same gate on a STRUCT FIELD (also a declarator-mode row with paramDecay=false).
TEST(SemanticAnalyzerC, ArrayStaticOnStructFieldFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ExternArrayStaticFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ArrayStaticOnGroupedInnerParamFailsLoud) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, DerefSizedVlaStillCompiles) {
    auto model = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, ArrayStarOuterDecaysInnerStarFailsLoud) {
    auto outer = analyzeShipped("c", {
        "int f(int a[*][3]) { return a[1][2]; }\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_FALSE(outer.hasErrors())
        << "`int a[*][3]` = an outer star-modifier + fixed inner: decays to `int(*)[3]` "
           "(a fixed inner stride), accepted exactly like `int a[][3]`";
    auto inner = analyzeShipped("c", {
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
TEST(SemanticAnalyzerC, MultiDimParamInnerStaticSizesCorrectly) {
    auto model = analyzeShipped("c", {
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

// D-CSUBSET-VLA-PTR-INIT-FORM-TYPING — ✅ CLOSED IN P34, AND THE DEFERRAL'S OWN
// SAFETY BOUNDARY IS WHAT CAUGHT IT. This test used to be
// `PtrToVlaInitFormDeferredStillFailsLoud`, asserting that `int (*p)[n] = b;` FAILS
// LOUD, and it warned in these words: "a future PARTIAL fix that makes `= b`
// assignable WITHOUT also fixing the body-typing wrinkle would silently convert
// this safe reject into a wrong-STRIDE miscompile at the subscript. This test goes
// RED on exactly that dangerous partial change." It DID go red, on
// D-CSUBSET-VLA-INITIALIZER's un-merge — and the change turned out to be the WHOLE
// fix, not a partial one, which was then proven by EXECUTION rather than argued.
//
// ★ THE ROW'S RECORDED ROOT CAUSE WAS WRONG, and that is why a cycle was spent on
// a fix that "proved INERT". It blamed the Pass-2 initializer stamp ("`b` is
// pre-stamped with its decayed type"). The real blocker was on the DECLARATOR
// side: with the flexible-array flag tested ABOVE the VLA arm and an initializer
// present, `(*p)[n]` built `Ptr<incompleteArray<int>>` — so `type_rules`'
// `Ptr<vlaArray> <- array(array)` decay compare could never match no matter what
// the initializer's stamp said, and `storePtrToVlaStride`'s `typeContainsVla`
// gate (which tests for the -2 sentinel) never fired either. Both halves of the
// deferral were one defect. `p` now declares as `Ptr<vlaArray<int>>`, the compare
// matches, and the runtime row stride is frozen at the decl exactly as the
// assignment form's already was.
//
// ★ THE ASSERTION IS THE OFF-DIAGONAL, DELIBERATELY. `p[1][0] == 20` (not 11 — a
// stride of one ELEMENT instead of a runtime ROW — and not 21 — a transposed
// read) is the only shape that discriminates a correct stride from a plausible
// wrong one. ✔MEASURED end-to-end through the shipped CLI in BOTH pipeline arms,
// and against gcc 13.3.0 and clang 19.1.1: all four exit 42
// (`examples/c/c99_vla_ptr_init` carries the runnable witness).
TEST(SemanticAnalyzerC, PtrToVlaInitFormIsAcceptedAndKeepsItsVlaPointee) {
    auto model = analyzeShipped("c", {
        "int main(void) {\n"
        "  volatile int vn = 3;\n"
        "  int n = vn;\n"
        "  int b[2][n];\n"
        "  int (*p)[n] = b;\n"          // the INIT form — accepted since P34
        "  return p[1][0];\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "the pointer-to-VLA INIT form `int (*p)[n] = b` is the natural spelling and "
           "both references accept it";
    auto const* p = findSymbolNamed(model, "p");
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(p->type.valid());
    TypeInterner const& in = model.lattice().interner();
    ASSERT_EQ(in.kind(p->type), TypeKind::Ptr);
    auto const pops = in.operands(p->type);
    ASSERT_FALSE(pops.empty());
    EXPECT_TRUE(in.isVlaArray(pops[0]))
        << "the POINTEE must stay the VLA row — an accepted init form whose pointee "
           "quietly became an incomplete array is the wrong-stride miscompile the "
           "deferral's boundary pin existed to catch";
    EXPECT_FALSE(in.isIncompleteArray(pops[0]))
        << "and specifically not the incomplete array the merged flag produced";
}

// ─── FC17.9(e) (D-CSUBSET-LONG-DOUBLE): the per-format long-double axis ──────
//
// `long double`'s REPRESENTATION is ABI-divergent per object format (64-bit
// IEEE on pe64/apple-arm64, x87 80-bit on SysV/darwin x86_64, binary128 on
// linux-arm64), so the c typeSpecifiers row carries a
// coreByLongDoubleFormat map resolved against `analyze()`'s LongDoubleFormat
// axis: f64 → F64 (long double IS double — the full machinery serves it),
// x87-80 → F80, ieee128 → F128, and an UNDECLARED axis (None — wasm/spirv/
// direct-API) leaves the row UNREALIZED → the precise
// S_LongDoubleFormatUndeclared, NEVER a silently-guessed base core.

// (Symbol lookup reuses the file-wide `findSymbolNamed` helper above.)
namespace {
[[nodiscard]] SemanticModel analyzeWithLongDoubleAxis(
    std::initializer_list<std::string> sources, LongDoubleFormat axis) {
    auto cu = buildShippedUnit("c", sources);
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, std::nullopt, std::nullopt,
                   std::nullopt, std::nullopt, axis);
}
} // namespace

TEST(SemanticAnalyzerC, LongDoubleResolvesPerAxis) {
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

TEST(SemanticAnalyzerC, LongDoubleUndeclaredAxisFailsLoud) {
    // Default analyze() = LongDoubleFormat::None (direct-API / wasm / spirv):
    // the row is UNREALIZED — the PRECISE 0xE056, not the generic S0011, and
    // NEVER a silent F64 bind (the base-core-fallback trap, IMPORTANT-4).
    auto model = analyzeShipped("c", {
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

TEST(SemanticAnalyzerC, LongDoubleIsDoubleOnF64Axis) {
    // The f64 axis (pe64 / apple-arm64): `long double` has double's
    // REPRESENTATION (F64). It remains a DISTINCT TYPE
    // (D-LANG-TYPE-IDENTITY-VOCABULARY — identity is the vocabulary entry,
    // never the core), and the conversion between the two is a LEGAL IMPLICIT
    // conversion in both directions, so the whole double machinery serves it
    // with zero new codegen (the same-representation conversion re-tags; it
    // emits no Cast).
    auto model = analyzeWithLongDoubleAxis(
        {"int main(void) { long double x; double d; x = d; d = x; x = 1.5; }\n"},
        LongDoubleFormat::F64);
    EXPECT_FALSE(model.hasErrors())
        << "on the f64 axis `long double` IS double — both assignment "
           "directions must be clean";
}

TEST(SemanticAnalyzerC, LongDoubleLiteralTypesPerAxis) {
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
    auto none = analyzeShipped("c", {
        "int main(void) { double d = 20.0L; }\n",
    });
    EXPECT_EQ(countCode(none.diagnostics(),
                        DiagnosticCode::S_LongDoubleFormatUndeclared), 1u)
        << "a `20.0L` literal under an undeclared axis must emit 0xE056";
}

TEST(SemanticAnalyzerC, LongDoubleUsualArithmeticConversionOutranksDouble) {
    // C 6.3.1.8: long double outranks double (floatRank F80=4 > F64=3), so `x + d`
    // (long double + double) types long double (F80). NOTE: this test formerly proved
    // the rank via an assignment-rejection PROXY (`double r = x + d;` had to REJECT the
    // F80->F64 narrowing). D-CSUBSET-FLOAT-FROM-DOUBLE-NARROWING now ADMITS that
    // narrowing, so the proxy is obsolete — the rank is pinned DIRECTLY via the INFERRED
    // TYPE of the UAC result: `typeof(x + d) pr;` declares `pr` with x+d's type, and the
    // symbol's resolved TypeKind must be F80 (the exact idiom LongDoubleResolvesPerAxis
    // uses). The `good` case still also binds an F80 lhs cleanly.
    auto good = analyzeWithLongDoubleAxis(
        {"int main(void) { long double x; double d; typeof(x + d) pr;\n"
         "  long double r; r = x + d; }\n"},
        LongDoubleFormat::X87_80);
    EXPECT_FALSE(good.hasErrors())
        << "`long double + double` binds an F80 lhs cleanly";
    auto const* pr = findSymbolNamed(good, "pr");
    ASSERT_NE(pr, nullptr);
    ASSERT_TRUE(pr->type.valid());
    EXPECT_EQ(good.lattice().interner().kind(pr->type), TypeKind::F80)
        << "`x + d` (long double + double) types long double (F80): typeof(x + d) "
           "resolves F80, proving the UAC result OUTRANKS double (F64)";

    // Positive control / RED-ON-DISABLE of the RANK: `double + double` types double, so
    // typeof(d1 + d2) resolves F64. Were the UAC rank broken so `x + d` above typed
    // double, its EXPECT_EQ(..., F80) would fail. Proves the oracle separates F64/F80.
    auto ctrl = analyzeWithLongDoubleAxis(
        {"int main(void) { double d1; double d2; typeof(d1 + d2) pr2; }\n"},
        LongDoubleFormat::X87_80);
    auto const* pr2 = findSymbolNamed(ctrl, "pr2");
    ASSERT_NE(pr2, nullptr);
    ASSERT_TRUE(pr2->type.valid());
    EXPECT_EQ(ctrl.lattice().interner().kind(pr2->type), TypeKind::F64)
        << "`double + double` types double (F64) — the typeof oracle separates the two "
           "float widths, so the F80 rank pin above is observable";
}

TEST(SemanticAnalyzerC, LongDoubleConstexprFoldSucceedsOnWalledAxis) {
    // LD-3 (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION): the const-eval fold gate
    // is RELAXED. F80/F128 now fold at TRUE target precision via the
    // `WideFloatValue` soft-float kernel, so `constexpr long double k = 20.0L +
    // 22.0L;` IS a valid compile-time constant on EVERY long-double axis — no
    // `S_ConstexprNonConstantInitializer`. INVERTED from the former refusal pin
    // (which asserted the walled axis walled the fold). The x87-80 (F80) axis:
    auto walled = analyzeWithLongDoubleAxis(
        {"int main(void) { constexpr long double k = 20.0L + 22.0L; }\n"},
        LongDoubleFormat::X87_80);
    EXPECT_EQ(countCode(walled.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 0u)
        << "F80 constexpr arithmetic must now FOLD (LD-3 target-precision kernel) "
           "— a non-constant diagnostic here means the fold gate is still walled";
    EXPECT_FALSE(walled.hasErrors())
        << "the x87-80 constexpr long double fold must analyze clean";

    // The ieee128 (F128) sibling — the SAME source folds via the binary128 kernel.
    auto ieee128 = analyzeWithLongDoubleAxis(
        {"int main(void) { constexpr long double k = 20.0L + 22.0L; }\n"},
        LongDoubleFormat::Ieee128);
    EXPECT_EQ(countCode(ieee128.diagnostics(),
                        DiagnosticCode::S_ConstexprNonConstantInitializer), 0u)
        << "F128 constexpr arithmetic must fold (LD-3 binary128 kernel)";
    EXPECT_FALSE(ieee128.hasErrors())
        << "the ieee128 constexpr long double fold must analyze clean";

    auto f64 = analyzeWithLongDoubleAxis(
        {"int main(void) { constexpr long double k = 20.0L + 22.0L; }\n"},
        LongDoubleFormat::F64);
    EXPECT_FALSE(f64.hasErrors())
        << "on the f64 axis the SAME fold is exact (long double IS binary64) "
           "— must fold clean";
}

// ── FC17.9(g) (D-CSUBSET-TGMATH): the SHIPPED <tgmath.h> type-generic macros ──
//
// These pins run against the REAL src/dss-config/shippedLibs/tgmath.json — NOT
// a scratch copy — so a regression IN THE SHIPPED FILE flips them red (the
// scratch-descriptor discipline would keep a stale mirror green while the
// shipped macro rotted). Each tgmath name is a function-like `_Generic` macro
// spliced by the preprocessor at `#include <tgmath.h>`: float → the f-variant
// with an explicit `(float)` cast; default → the BARE f64 function. Both cast
// directions are load-bearing (the descriptor $comment documents the empirical
// proofs); the pins here are:
//   * the 17-function × {float,double,int} COMPILE MATRIX stays clean on BOTH
//     an elf and a pe target (pe exercises the fabs/ldexp per-format `variants`
//     whose float arm bridges through the f64 fn — msvcrt exports no
//     fabsf/ldexpf, D-CSUBSET-MATH-FLOAT-VARIANTS-PE), with every float-arg
//     result assigned into a FLOAT lvalue — if a float arm ever mis-dispatched
//     to the f64 default, that assignment becomes an F64→F32 narrowing
//     S_TypeMismatch → RED (a per-function wrong-arm pin, not just "compiles");
//   * a `double _Complex` argument fails S_TypeMismatch LOUD through the BARE
//     default arm (D-CSUBSET-TGMATH-COMPLEX). RED-ON-DISABLE (verified during
//     the cycle): flip a default arm to `sqrt((double)(x))` in tgmath.json and
//     the complex call compiles SILENTLY — (double)z is legal C, drops the
//     imaginary part, a conformance MISCOMPILE — which is exactly what these
//     count pins catch.

namespace {

namespace fs = std::filesystem;

// Resolve the REAL shipped system-include dir (src/dss-config/shippedLibs)
// through the ONE test-side resolver (`repo_root.hpp`: $DSS_CONFIG_ROOT → the
// repo root CMake bakes in → a cwd ancestor walk), so the pins exercise the
// descriptor the production driver ships.
//
// This was a private 8-level upward walk mirroring program.cpp's applySystemDirs
// — and it read neither of the first two sources, so an out-of-tree build (whose
// cwd has no `src/dss-config` in its ancestry) missed on every hop, returned
// `{}`, and each of the four callers below answered with `std::abort()`.
// `abort()` kills the whole test BINARY, so ONE unresolvable directory cost
// every other test in this file its verdict — the harness could not even say
// which unit was unhappy. This THROWS instead, which GoogleTest reports as a
// failure of the one test that asked; the four `.empty()`/`abort()` guards are
// gone with it because the path can no longer come back empty.
[[nodiscard]] fs::path findRealShippedLibsDir() {
    fs::path const dir = dss::test::configRoot() / "shippedLibs";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        throw std::runtime_error(
            "the shipped-lib descriptor directory is missing: " + dir.string());
    }
    return dir;
}

// Build + analyze `mainSrc` with the REAL shippedLibs dir on the system path
// and `format` as the ACTIVE object-format — required by the fabs/ldexp macro
// `variants` (format-keyed splice, the setjmp.json precedent) and the
// fabsf/ldexpf per-symbol availability gate. Mirrors the production driver's
// per-format CU build (UnitBuilder::setActiveFormat + analyze(activeFormat, DiagnosticBudget::libraryDefault())).
[[nodiscard]] SemanticModel analyzeRealTgmath(std::string mainSrc,
                                              ObjectFormatKind format,
                                              DataModel dataModel) {
    fs::path const shipped = findRealShippedLibsDir();   // throws if unresolvable
    auto schema = loadShippedSchema("c");
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addSystemDir(shipped);
    builder.setActiveFormat(format);
    builder.addInMemory(std::move(mainSrc), "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), dataModel, std::nullopt, std::nullopt, format, "x86_64");
}

// The 17-function × {float,double,int} matrix. Every float-column result is
// assigned into FLOAT `s` — the strict wrong-arm pin: a float arm that
// mis-dispatches to the f64 default makes the assignment an F64→F32 narrowing
// S_TypeMismatch. The double/int columns assign into DOUBLE `r` (the f64
// default's return; int rides `default:` via int→f64 implicit widening,
// C 7.25p3). Mixed two-arg combos (any non-float arg) route to the f64 fn.
constexpr char const* kTgmathMatrixSrc =
    "#include <tgmath.h>\n"
    "int main(void) {\n"
    "    float f; double d; int i; float s; double r;\n"
    "    f = 1.0f; d = 1.0; i = 1;\n"
    "    s = sqrt(f); s = sin(f); s = cos(f); s = tan(f); s = asin(f);\n"
    "    s = acos(f); s = atan(f); s = exp(f); s = log(f); s = log10(f);\n"
    "    s = floor(f); s = ceil(f); s = fabs(f); s = pow(f, f);\n"
    "    s = atan2(f, f); s = fmod(f, f); s = ldexp(f, i);\n"
    "    r = sqrt(d); r = sin(d); r = cos(d); r = tan(d); r = asin(d);\n"
    "    r = acos(d); r = atan(d); r = exp(d); r = log(d); r = log10(d);\n"
    "    r = floor(d); r = ceil(d); r = fabs(d); r = pow(d, d);\n"
    "    r = atan2(d, d); r = fmod(d, d); r = ldexp(d, i);\n"
    "    r = sqrt(i); r = sin(i); r = cos(i); r = tan(i); r = asin(i);\n"
    "    r = acos(i); r = atan(i); r = exp(i); r = log(i); r = log10(i);\n"
    "    r = floor(i); r = ceil(i); r = fabs(i); r = pow(i, i);\n"
    "    r = atan2(i, i); r = fmod(i, i); r = ldexp(i, i);\n"
    "    r = pow(f, d); r = pow(d, f); r = pow(f, i);\n"
    "    r = atan2(i, f); r = fmod(d, f);\n"
    "    return 0;\n"
    "}\n";

} // namespace

// The matrix on an ELF target: the float arms bind the REAL fabsf/ldexpf
// imports (per-symbol availableObjectFormats admits elf) and the elf macro
// variants. ZERO diagnostics of any kind.
TEST(SemanticAnalyzerC, TgmathMatrixCleanOnElf) {
    auto model = analyzeRealTgmath(kTgmathMatrixSrc,
                                   ObjectFormatKind::Elf, DataModel::Lp64);
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "a float-column S_TypeMismatch here means a float arm mis-dispatched "
           "to the f64 default (F64->F32 narrowing at the `s =` assignment)";
}

// The SAME matrix on a PE target: msvcrt exports no fabsf/ldexpf, so the
// fabs/ldexp macros' pe `variants` arm routes their float arm THROUGH the f64
// function — `(float)fabs((double)(x))` — typed float (the `s = fabs(f)`
// assignment still requires a FLOAT result). RED if the pe variant arm is
// dropped (the flat elf body would reference the pe-unavailable fabsf) or if
// the bridge loses its `(float)` result cast (F64->F32 narrowing).
TEST(SemanticAnalyzerC, TgmathMatrixCleanOnPeViaVariantBridge) {
    auto model = analyzeRealTgmath(kTgmathMatrixSrc,
                                   ObjectFormatKind::Pe, DataModel::Llp64);
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}

// D-CSUBSET-TGMATH-COMPLEX: a `double _Complex` argument through the shipped
// sqrt macro fails EXACTLY one S_TypeMismatch — the SELECTED bare default arm
// `sqrt((z))` passes a complex value to the f64 param (loud); the UNSELECTED
// float arm's `(float)(z)` cast type-checks but never lowers. RED-ON-DISABLE:
// rewrite the default arm as `sqrt((double)(x))` and this compiles clean —
// the cast launders the complex arg (drops imag), the exact conformance
// miscompile the bare arm exists to prevent.
TEST(SemanticAnalyzerC, TgmathComplexArgSqrtFailsLoud) {
    auto model = analyzeRealTgmath(
        "#include <tgmath.h>\n"
        "int main(void) { double _Complex z; double r; z = 4.0;\n"
        "                 r = sqrt(z); return (int)r; }\n",
        ObjectFormatKind::Elf, DataModel::Lp64);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
        << "sqrt(double _Complex) must fail LOUD through the bare default arm "
           "— zero means the arm laundered the complex arg ((double)(x) crept "
           "back in): a silent drop-imag miscompile";
}

// The two-arg nested `_Generic` (pow) with a complex first arg: TWO
// S_TypeMismatch — the SELECTED outer default `pow((z),(y))` plus the
// UNSELECTED-but-type-checked inner default arm (same bare-call text inside
// the float branch). Both are the SAME loudness guarantee; the count is
// pinned so a silent-arm regression (either bare call gaining a cast) drops
// the count and flips this red.
TEST(SemanticAnalyzerC, TgmathComplexArgPowFailsLoudTwice) {
    auto model = analyzeRealTgmath(
        "#include <tgmath.h>\n"
        "int main(void) { double _Complex z; double r; z = 4.0;\n"
        "                 r = pow(z, 2.0); return (int)r; }\n",
        ObjectFormatKind::Elf, DataModel::Lp64);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 2u)
        << "pow(double _Complex, double) must fail LOUD through both bare "
           "default arms (selected outer + type-checked inner)";
}

// fabs — the per-format `variants` macro — keeps the SAME complex loudness on
// BOTH realizations: the elf arm's `fabs((x))` and the pe BRIDGE arm's
// `fabs((x))` default are equally bare. One S_TypeMismatch each.
TEST(SemanticAnalyzerC, TgmathComplexArgFabsFailsLoudOnBothFormats) {
    constexpr char const* src =
        "#include <tgmath.h>\n"
        "int main(void) { double _Complex z; double r; z = 4.0;\n"
        "                 r = fabs(z); return (int)r; }\n";
    auto elf = analyzeRealTgmath(src, ObjectFormatKind::Elf, DataModel::Lp64);
    EXPECT_EQ(countCode(elf.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
        << "fabs(double _Complex) must fail LOUD on elf (fabsf variant arm)";
    auto pe = analyzeRealTgmath(src, ObjectFormatKind::Pe, DataModel::Llp64);
    EXPECT_EQ(countCode(pe.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
        << "fabs(double _Complex) must fail LOUD on pe (the f64 bridge arm)";
}

// ─────────────────────────────────────────────────────────────────────────────
// TF-C73 (D-CSUBSET-GNU-ATTRIBUTE): multi-root + multi-clause attribute scanning,
// and the GNU `aligned(N)` sink.
//
// Every C snippet below was checked with
//   clang -fsyntax-only -Wall -Wextra -isysroot $(xcrun --show-sdk-path)
// and every layout number pinned here is clang's OWN measured answer, not a
// guess — `struct S { char a; int b; } __attribute__((packed, aligned(16)));`
// really is sizeof 16 / _Alignof 16 on arm64.
// ─────────────────────────────────────────────────────────────────────────────

// ★★ THE LIVE SILENT MISCOMPILE (work item 7). `scanCompositePacked` used to do
// `if (named) { packed = true; continue; }` over a WHOLE attribute node, so the
// first clause naming `packed` SWALLOWED every clause after it. Measured before
// the fix: DSS `sizeof` 5 against clang's 16 with ZERO diagnostics.
//
// The MIRROR of that swallow is what this test pins, because it is the half that
// is fully fixable inside the semantic tier: an unknown name sitting AFTER
// `packed` used to be accepted in total silence, defeating the typo protection
// this scan exists for. It now fails loud, and the struct is STILL packed (the
// recognized clause is honored, the unrecognized one is reported — neither
// clause swallows the other).
TEST(SemanticAnalyzerC, PackedMultiClauseUnknownAfterPackedFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "struct S { char a; int b; } __attribute__((packed, bogus_xyz));\n"
        "_Static_assert(sizeof(struct S) == 5, \"still packed\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    // Exactly ONE diagnostic, and it NAMES the offending clause — not a bare
    // count on a shared code, and not the whole `__attribute__((...))` blob.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 1u)
        << "a clause after `packed` must still be examined — the pre-TF-C73 "
           "scan swallowed it and accepted the typo silently";
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_UnknownTypeAttribute) continue;
        EXPECT_EQ(d.severity, DiagnosticSeverity::Error);
        EXPECT_NE(d.actual.find("bogus_xyz"), std::string::npos)
            << "the diagnostic must name the unrecognized clause, got: " << d.actual;
    }
    // The recognized clause is still honored: packed survives.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "`packed` must still apply when a sibling clause is unknown";
}

// The same swallow in the other clause ORDER: an unknown name BEFORE `packed`.
// Pre-TF-C73 this was also silent (the subtree scan found `packed` anywhere in
// the node and returned early). Both orders now behave identically — which is
// the point: correctness must not depend on clause position.
TEST(SemanticAnalyzerC, PackedMultiClauseUnknownBeforePackedFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "struct S { char a; int b; } __attribute__((bogus_xyz, packed));\n"
        "_Static_assert(sizeof(struct S) == 5, \"still packed\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_UnknownTypeAttribute) continue;
        EXPECT_NE(d.actual.find("bogus_xyz"), std::string::npos) << d.actual;
    }
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// REGRESSION WALL for the clause-by-clause rewrite: a SINGLE-clause
// `__attribute__((packed))` must still be exactly as it was — packed honored,
// no diagnostic. (The multi-clause enumeration must not change the common case.)
TEST(SemanticAnalyzerC, PackedSingleClauseUnchangedByClauseEnumeration) {
    auto cu = buildShippedUnit("c", {
        "struct S { char c; int v; } __attribute__((packed));\n"
        "_Static_assert(sizeof(struct S) == 5, \"packed size 5\");\n"
        "_Static_assert(_Alignof(struct S) == 1, \"packed align 1\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// ★ THE PHANTOM-CLAUSE EXCLUSION (work item 8). `format(printf,1,2)` nests its
// argument identifier `printf` in an `attrArgAtom` whose direct child IS an
// identifier — exactly the shape the trailing-clause rule looks for. Without the
// `cfg.attributeArgRule` skip in `collectAttrClauses`, the walk would mint a
// PHANTOM clause named `printf` and fail it loud on perfectly legal C.
// The position is a TYPEDEF deliberately: `typedefDecl` sets
// `unknownStrictAttributeIsError`, so a phantom clause there is not merely
// ignored — it fails LOUD. That is what makes this pin actually red-on-disable
// (on a row WITHOUT the strict gate an unknown GNU name is silent, and the test
// would pass for the wrong reason).
// RED-ON-DISABLE: delete the `attributeArgRule` guard in `collectAttrClauses`
// and `printf` is minted as a clause → S_UnknownTypeAttribute on legal C.
TEST(SemanticAnalyzerC, AttrArgIdentifierNeverMintsPhantomClause) {
    auto cu = buildShippedUnit("c", {
        "typedef void (*fp)(const char *, ...) "
        "__attribute__((format(printf,1,2)));\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u)
        << "`printf` is an ARGUMENT, not a clause name — minting a phantom clause "
           "would reject legal C";
    for (auto const& d : model.diagnostics().all()) {
        EXPECT_EQ(d.actual.find("printf"), std::string::npos)
            << "an ARGUMENT identifier must never be read as a clause NAME, got: "
            << d.actual;
    }
}

// ★ `[[a, b]]` IS TWO CLAUSES, EACH FOLDED ONCE (work item 8 audit caveat). The
// C23 form's per-item loop is the ONLY producer for that shape; the structural
// trailing-clause descent is deliberately skipped for it (`collectAttrClauses`
// returns immediately from the std arm) so no item is yielded twice.
//
// ★ MEASURED CAVEAT, recorded so nobody re-derives it: a genuine double-FOLD of
// the same clause node is NOT observable through diagnostic counts, because
// `DiagnosticReporter` carries a `dedupWindow` (default 4) that drops an
// identical (code, buffer, span, rule, actual) diagnostic — and re-folding the
// same node produces exactly that. Fact folding is idempotent too (booleans OR,
// messages are first-non-empty-wins, alignment is a MAX). So this test pins the
// direction that IS observable and does regress: BOTH items must be folded —
// removing the per-item loop takes the count to 0.
TEST(SemanticAnalyzerC, StdAttrMultiItemEmitsNoDuplicateDiagnostics) {
    auto cu = buildShippedUnit("c", {
        "[[frobnicate, blahblah]] int x;\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownAttribute), 2u)
        << "[[a, b]] is TWO clauses — one warning each, never doubled";
}

// ★★ PER-DECLARATOR ISOLATION (work item 1). `int a __attribute__((deprecated)), b;`
// annotates ONLY `a`. If the declarator-depth scan folded into the SHARED
// declaration-level facts instead of a per-declarator COPY, `b` would silently
// inherit the deprecation — a wrong fact on a symbol the programmer never marked.
// This is the test that distinguishes a copy from a reference.
TEST(SemanticAnalyzerC, AfterDeclaratorAttrDoesNotLeakToSiblingDeclarator) {
    auto model = analyzeShipped("c", {
        "int a __attribute__((deprecated)), b;\n"
        "int main(void) { return a + b; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u)
        << "only `a` is deprecated — folding into shared facts would mark `b` too";
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_DeprecatedSymbolUsed) continue;
        EXPECT_EQ(d.actual, "a") << "the deprecation must land on `a`, never `b`";
    }
    SymbolRecord const* bRec = findSym(model, "b");
    ASSERT_NE(bRec, nullptr);
    EXPECT_FALSE(bRec->isDeprecated)
        << "`b` carries no attribute and must not be marked";
}

// ROOT (b) — DECLARATOR-DEPTH. An attribute written AFTER the declarator used to
// be parsed and silently ignored (the scan had exactly one root: the specifier
// prefix). RED-ON-DISABLE: remove the after-declarator root loop and the
// deprecation warning disappears entirely.
TEST(SemanticAnalyzerC, AfterDeclaratorAttrIsHonored) {
    auto model = analyzeShipped("c", {
        "int x __attribute__((deprecated));\n"
        "int main(void) { return x; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 1u)
        << "an after-declarator attribute must be honored, not parse-and-ignore";
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_DeprecatedSymbolUsed) continue;
        EXPECT_EQ(d.actual, "x");
        EXPECT_EQ(d.severity, DiagnosticSeverity::Warning);
    }
}

// The after-declarator MESSAGE decodes too (the shared string chokepoint is
// reached from the new root exactly as from the prefix root).
TEST(SemanticAnalyzerC, AfterDeclaratorAttrMessageDecodes) {
    auto model = analyzeShipped("c", {
        "int x __attribute__((deprecated(\"use y\")));\n"
        "int main(void) { return x; }\n",
    });
    bool sawMessage = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_DeprecatedSymbolUsed) continue;
        sawMessage = true;
        EXPECT_NE(d.actual.find("use y"), std::string::npos)
            << "the message must survive the declarator-depth root, got: " << d.actual;
    }
    EXPECT_TRUE(sawMessage);
}

// ── TF-C73: the GNU `aligned(N)` sink ────────────────────────────────────────
// Before this cycle `aligned` had NO sink at all: it parsed, folded to nothing,
// and the object was SILENTLY UNDER-ALIGNED. (That is why `aligned` was
// deliberately kept OUT of the linkage-scan hint-ignore list — ignoring it would
// have made the miscompile compile clean.)

// OBJECT (leading position) → SymbolRecord.explicitAlignment. An APPLIED FACT,
// not a diagnostic count. RED-ON-DISABLE: drop the Align arm and this is nullopt.
TEST(SemanticAnalyzerC, GnuAlignedLeadingSetsObjectExplicitAlignment) {
    auto cu = buildShippedUnit("c",
                               { "__attribute__((aligned(32))) int gv;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    SymbolRecord const* gv = findSym(model, "gv");
    ASSERT_NE(gv, nullptr);
    ASSERT_TRUE(gv->explicitAlignment.has_value())
        << "__attribute__((aligned(32))) must set explicitAlignment — an "
           "unhonored alignment is a silently under-aligned object";
    EXPECT_EQ(*gv->explicitAlignment, 32u);
    EXPECT_FALSE(model.hasErrors());
}

// OBJECT (after-declarator position) → the SAME sink, reached through the NEW
// declarator-depth root. Both spellings must agree.
TEST(SemanticAnalyzerC, GnuAlignedAfterDeclaratorSetsObjectAlignment) {
    auto cu = buildShippedUnit("c",
                               { "int gv2 __attribute__((aligned(32)));\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    SymbolRecord const* gv2 = findSym(model, "gv2");
    ASSERT_NE(gv2, nullptr);
    ASSERT_TRUE(gv2->explicitAlignment.has_value());
    EXPECT_EQ(*gv2->explicitAlignment, 32u);
}

// `aligned` MAX-folds with `alignas` on the same declaration (C 6.7.5p6: the
// strictest wins). They write the SAME slot, so they cannot disagree.
TEST(SemanticAnalyzerC, GnuAlignedMaxFoldsWithAlignas) {
    auto cu = buildShippedUnit("c",
                               { "alignas(8) int g __attribute__((aligned(64)));\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    SymbolRecord const* g = findSym(model, "g");
    ASSERT_NE(g, nullptr);
    ASSERT_TRUE(g->explicitAlignment.has_value());
    EXPECT_EQ(*g->explicitAlignment, 64u)
        << "6.7.5p6: with several alignment specifiers the LARGEST wins";
}

// MEMBER → the existing `fieldAligns` path, end-to-end through the interner.
// clang measures sizeof 16 / _Alignof 8 for this struct; so must DSS.
TEST(SemanticAnalyzerC, GnuAlignedOnMemberRaisesLayoutEndToEnd) {
    auto cu = buildShippedUnit("c", {
        "struct M { char c; int v __attribute__((aligned(8))); };\n"
        "_Static_assert(sizeof(struct M) == 16, \"member aligned 8\");\n"
        "_Static_assert(_Alignof(struct M) == 8, \"member raises struct align\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "a member `aligned(8)` must reach fieldAligns and change the layout";
}

// MULTI-CLAUSE FOLD (work item 8): `((deprecated, aligned(32)))` — BOTH clauses
// take effect. Pre-TF-C73 only the FIRST was folded, so the alignment was
// silently dropped. RED-ON-DISABLE: remove the trailing-clause descent in
// `collectAttrClauses` and `explicitAlignment` goes nullopt while `deprecated`
// still works — which is exactly the asymmetry that made the drop invisible.
TEST(SemanticAnalyzerC, GnuMultiClauseFoldsEveryClause) {
    auto model = analyzeShipped("c", {
        "int q __attribute__((deprecated, aligned(32)));\n"
        "int main(void) { return q; }\n",
    });
    SymbolRecord const* q = findSym(model, "q");
    ASSERT_NE(q, nullptr);
    EXPECT_TRUE(q->isDeprecated) << "clause 1 must fold";
    ASSERT_TRUE(q->explicitAlignment.has_value())
        << "clause 2 must fold too — folding only the first silently drops it";
    EXPECT_EQ(*q->explicitAlignment, 32u);
}

// The reversed order folds identically — clause position must not matter.
TEST(SemanticAnalyzerC, GnuMultiClauseOrderIndependent) {
    auto model = analyzeShipped("c", {
        "int r __attribute__((aligned(32), deprecated));\n"
        "int main(void) { return r; }\n",
    });
    SymbolRecord const* r = findSym(model, "r");
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->isDeprecated);
    ASSERT_TRUE(r->explicitAlignment.has_value());
    EXPECT_EQ(*r->explicitAlignment, 32u);
}

// ★ THE SHARED-LADDER PROOF (work item 4). `aligned(3)` and `alignas(3)` must
// produce the SAME code, because they run the SAME function. This is what makes
// the extraction load-bearing rather than cosmetic: bypassing
// `foldAlignmentOperand` (hand-rolling an equivalent fold in the Align arm)
// makes the pow2 and >256 arms go SILENT for the attribute spelling while the
// alignas spelling keeps working — an asymmetry no alignas test can see.
TEST(SemanticAnalyzerC, GnuAlignedSharesTheAlignasValidationLadder) {
    {   // not a power of two — the SAME code alignas(3) emits
        auto cu = buildShippedUnit("c",
                                   { "int x __attribute__((aligned(3)));\n" });
        auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_AlignasNotPowerOfTwo), 1u)
            << "aligned(3) must fail through the SHARED ladder, not silently";
    }
    {   // over the 256 cap — the SAME code alignas(512) emits
        auto cu = buildShippedUnit("c",
                                   { "int x __attribute__((aligned(512)));\n" });
        auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_AlignasExceedsMax), 1u)
            << "aligned(512) must hit the SAME >256 cap alignas does";
    }
    {   // the alignas TWIN — proves the two spellings agree, code for code
        auto cu = buildShippedUnit("c", { "alignas(3) int x;\n" });
        auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_AlignasNotPowerOfTwo), 1u);
    }
}

// ★ KNOWN DIVERGENCE, PINNED DELIBERATELY SO IT IS VISIBLE RATHER THAN FOLKLORE.
// `aligned(0)` currently reaches the SHARED ladder, whose zero arm implements
// C 6.7.5p3 — "an alignment specification of zero has no effect" — and returns
// "no override" with NO diagnostic. That paragraph governs C11 `_Alignas`; it does
// NOT govern the GNU spelling, and clang REJECTS `__attribute__((aligned(0)))`
// with "requested alignment is not a power of 2" (MEASURED:
// `clang -fsyntax-only -Wall -Wextra`, exit 1).
//
// The behavior is a direct consequence of the design requirement that the GNU arm
// call the alignas ladder unchanged — zero behavior change on the alignas path is
// the regression wall, so the zero arm could not be special-cased for one caller
// without splitting the ladder the extraction exists to unify. The consequence is
// recorded here honestly: today `aligned(0)` is accepted and applies nothing, which
// is the one place in this feature where DSS is LOOSER than clang. Closing it means
// teaching the shared ladder that its zero arm is spelling-dependent (a `zeroIsNoOp`
// parameter set true only by `evalOneAlignasSpec`) — a deliberate follow-up, not an
// accident. This test PINS the current answer so the change is a visible test edit.
TEST(SemanticAnalyzerC, GnuAlignedZeroIsTheSharedNoOp) {
    auto cu = buildShippedUnit("c",
                               { "int z __attribute__((aligned(0)));\n" });
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNotPowerOfTwo), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasExceedsMax), 0u);
    SymbolRecord const* z = findSym(model, "z");
    ASSERT_NE(z, nullptr);
    EXPECT_FALSE(z->explicitAlignment.has_value())
        << "6.7.5p3: an alignment of zero has NO effect";
}

// ★ BARE `__attribute__((aligned))` FAILS LOUD. gcc reads it as "the target's
// maximum useful alignment" — a target-dependent number this engine must not
// invent. clang ACCEPTS this spelling, so DSS is deliberately stricter here:
// refusing loudly beats guessing an ABI.
TEST(SemanticAnalyzerC, GnuAlignedWithNoArgumentFailsLoud) {
    auto cu = buildShippedUnit("c",
                               { "int bare __attribute__((aligned));\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 1u);
    bool named = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_UnknownTypeAttribute) continue;
        EXPECT_EQ(d.severity, DiagnosticSeverity::Error);
        EXPECT_NE(d.actual.find("explicit alignment argument"), std::string::npos)
            << "the message must say WHY the bare form is refused, got: " << d.actual;
        named = true;
    }
    EXPECT_TRUE(named);
    SymbolRecord const* bare = findSym(model, "bare");
    ASSERT_NE(bare, nullptr);
    EXPECT_FALSE(bare->explicitAlignment.has_value())
        << "never guess 'maximum useful alignment'";
}

// FUNCTIONS stay LOUD (measured cost: 0 of 204 SDK `aligned` sites).
TEST(SemanticAnalyzerC, GnuAlignedOnFunctionFailsLoud) {
    auto cu = buildShippedUnit("c",
                               { "__attribute__((aligned(16))) int f(void);\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasInvalidContext), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_AlignasInvalidContext) continue;
        EXPECT_NE(d.actual.find("function"), std::string::npos) << d.actual;
    }
}

// ── ★ THE TYPEDEF RULE (work item 6) ─────────────────────────────────────────
// A typedef interns to the SAME TypeId as its aliasee, so `explicitAlignment` on
// a typedef symbol is provably inert — storing it would be the "parses but sets
// nothing" silent drop. So the request is JUDGED, in three graded arms.

// ARM 1 — N > natural, layout params AVAILABLE ⇒ FAIL LOUD. The program asked for
// stricter alignment than the alias can deliver, and we genuinely cannot deliver it.
TEST(SemanticAnalyzerC, GnuAlignedTypedefStricterThanNaturalFailsLoud) {
    auto cu = buildShippedUnit("c",
                               { "typedef int ti __attribute__((aligned(16)));\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasInvalidContext), 1u)
        << "aligned(16) on a typedef of int (natural 4) is a REAL drop — say so";
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_AlignasInvalidContext) continue;
        EXPECT_NE(d.actual.find("typedef"), std::string::npos) << d.actual;
    }
}

// ARM 2 — N <= natural, layout params AVAILABLE ⇒ accept SILENTLY. The alias
// already satisfies the request, so honoring it is a PROVEN no-op: nothing is
// dropped, so there is nothing to warn about. (Warning here would be noise on
// legal, already-satisfied C.)
TEST(SemanticAnalyzerC, GnuAlignedTypedefWeakerThanNaturalIsSilent) {
    auto cu = buildShippedUnit("c",
                               { "typedef int ts __attribute__((aligned(2)));\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasInvalidContext), 0u)
        << "aligned(2) on a typedef of int (natural 4) is ALREADY satisfied";
    EXPECT_FALSE(model.hasErrors());
}

// ARM 3 — ★ layout params ABSENT ⇒ STAY SILENT. This deliberately diverges from
// the alignas precedent (which fails loud when it cannot compute an alignment).
// `src/lsp/lsp_server.cpp` calls `dss::analyze(cu, DiagnosticBudget::libraryDefault())` with NO layout params, so
// failing loud here would put a red squiggle under every real SDK typedef in the
// editor while the same source compiles clean from the CLI.
// CANNOT-DETERMINE MUST NOT BECOME CANNOT-COMPILE.
TEST(SemanticAnalyzerC, GnuAlignedTypedefWithoutLayoutParamsStaysSilent) {
    auto model = analyzeShipped("c", {
        "typedef int ti __attribute__((aligned(16)));\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasInvalidContext), 0u)
        << "no layout params = cannot determine — that must not become "
           "cannot-compile (the LSP calls analyze() with no layout)";
    EXPECT_FALSE(model.hasErrors());
}

// ★ THE STRICT UNKNOWN-NAME GATE (work item 3). A `typedefDecl` declares no
// `linkageSpecifiers`, so `linkageFrom` early-returns and an unknown GNU name in
// a typedef position was reported by NOBODY — `typedef __attribute__((desprecated))
// int T;` compiled clean with the decoration silently unapplied. With the row's
// `unknownStrictAttributeIsError` opt-in it is now an ERROR, using the SAME code
// and severity `scanCompositePacked` already uses for a strict unknown.
// RED-ON-DISABLE: drop the `strictUnknownIsError` arm → count goes to 0.
TEST(SemanticAnalyzerC, TypedefUnknownStrictGnuAttributeFailsLoud) {
    auto cu = buildShippedUnit("c",
                               { "typedef __attribute__((desprecated)) int T;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 1u)
        << "a typo'd GNU attribute on a typedef must not be silently unapplied";
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_UnknownTypeAttribute) continue;
        EXPECT_EQ(d.severity, DiagnosticSeverity::Error);
        EXPECT_NE(d.actual.find("desprecated"), std::string::npos) << d.actual;
    }
}

// The C23 `[[...]]` form on the SAME row keeps its SUPPRESSIBLE Warning — C23
// REQUIRES an unknown standard attribute to be ignorable, so the strict gate must
// not leak across forms. This is the control that proves the gate is GNU-only.
//
// The placement is the typedef's MIDDLE slot (`typedef int [[…]] T;`), which is
// both valid C23 (clang -std=c2x accepts it, warning only about the unknown name)
// and the SAME scan root the GNU strict test above uses — so the only variable
// between the two tests is the attribute FORM, which is exactly what is being
// controlled for. (`typedef [[…]] int T;` — attribute before the type — is NOT
// valid C: clang rejects it with "an attribute list cannot appear here".)
TEST(SemanticAnalyzerC, TypedefUnknownC23AttributeStaysASuppressibleWarning) {
    auto cu = buildShippedUnit("c", { "typedef int [[frobnicate]] T;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u)
        << "C23 forbids a fatal unknown standard attribute — the strict gate is "
           "GNU-only and must not leak across forms";
    for (auto const& d : model.diagnostics().all())
        if (d.code == DiagnosticCode::S_UnknownAttribute)
            EXPECT_EQ(d.severity, DiagnosticSeverity::Warning);
}

// ROOT (a) — DECLARATION-DEPTH SLOTS. `typedef int64_t __attribute__((__aligned__(8)))
// T;` puts the attribute in the typedef's MIDDLE slot — outside `specifierPrefixChild`'s
// reach, so pre-TF-C73 it was parsed and silently ignored. RED-ON-DISABLE: remove the
// `declarationAttrSlotRules` loop and the loud typedef judgement disappears.
TEST(SemanticAnalyzerC, TypedefMiddleSlotAttributeIsReached) {
    auto cu = buildShippedUnit("c",
                               { "typedef int __attribute__((aligned(16))) T;\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasInvalidContext), 1u)
        << "the typedef's MIDDLE attribute slot must be a scan root — silently "
           "ignoring it is how the SDK's dominant typedef spelling got dropped";
}

// ★★ THE COMPOSITE-DEFINITION `aligned` POSITION — NOW A REAL SINK
// (D-CSUBSET-COMPOSITE-ALIGNED). This test is the CONVERSION of TF-C73's
// `CompositeDefinitionAlignedFailsLoudPendingInternerChannel` pin, which asserted
// that the clause failed LOUD because there was nowhere to put it. The interner now
// carries a genuine per-COMPOSITE alignment channel
// (`TypeInterner::explicitCompositeAlign`, seeded into `StructLayout::align` by
// `computeLayout`), so the number is produced instead of refused.
//
// ★ CLANG GROUND TRUTH (MEASURED on this machine, arm64 macOS, compiled AND RUN,
// `clang -fsyntax-only -Wall -Wextra -isysroot $(xcrun --show-sdk-path)` clean):
//     struct S { char a; int b; } __attribute__((packed, aligned(16)));
//         → sizeof 16, _Alignof 16
// NOT 5: packed removes the inter-field padding (a@0, b@1, extent 5) and
// `aligned(16)` then raises the WHOLE aggregate's alignment, which rounds the size
// up to 16. Both channels apply; neither swallows the other.
//
// RED-ON-DISABLE: revert the `composedAlign.value_or(0u)` argument at the struct
// `completeComposite` call (semantic_analyzer.cpp) — or the `compositeSeedAlign`
// seed in type_layout.cpp's Struct arm — and this drops back to packed's bare
// sizeof 5 / _Alignof 1, firing BOTH _Static_asserts.
TEST(SemanticAnalyzerC, CompositePackedPlusAlignedRaisesWholeAggregate) {
    auto cu = buildShippedUnit("c", {
        "struct S { char a; int b; } __attribute__((packed, aligned(16)));\n"
        "_Static_assert(sizeof(struct S) == 16, \"clang: packed+aligned(16) = 16\");\n"
        "_Static_assert(_Alignof(struct S) == 16, \"clang: _Alignof 16\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "packed removes the padding, aligned(16) raises the aggregate: clang "
           "gives sizeof 16 / _Alignof 16, NOT packed's bare 5 / 1";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u)
        << "the composite `aligned` clause is HONORED now, not refused as unknown";
    EXPECT_FALSE(model.hasErrors());
}

// The `aligned(N)` half ALONE on a composite definition — no packed.
// CLANG (MEASURED, compiled and run): `struct A { char a; int b; }
// __attribute__((aligned(16)));` → sizeof 16, _Alignof 16 (natural would be 8 / 4).
// RED-ON-DISABLE: drop the layout seed and this falls to 8 / 4.
TEST(SemanticAnalyzerC, CompositeAlignedAloneRaisesAlignAndSize) {
    auto cu = buildShippedUnit("c", {
        "struct A { char a; int b; } __attribute__((aligned(16)));\n"
        "_Static_assert(sizeof(struct A) == 16, \"clang: 16\");\n"
        "_Static_assert(_Alignof(struct A) == 16, \"clang: 16\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// ★ `aligned(N)` NEVER LOWERS (C 6.7.5 — the composite's alignment is the MAX of its
// natural alignment and the request). CLANG (MEASURED): `struct L { char a; int b; }
// __attribute__((aligned(2)));` → sizeof 8, _Alignof 4 — the request is WEAKER than
// the natural 4 and is simply a no-op. This is the control that proves the fold is a
// MAX and not an assignment: implement it as `out.align = requested` and this test
// goes to sizeof 4 / _Alignof 2 while every other test in this group still passes.
TEST(SemanticAnalyzerC, CompositeAlignedWeakerThanNaturalIsANoOp) {
    auto cu = buildShippedUnit("c", {
        "struct L { char a; int b; } __attribute__((aligned(2)));\n"
        "_Static_assert(sizeof(struct L) == 8, \"clang: natural 8 survives\");\n"
        "_Static_assert(_Alignof(struct L) == 4, \"clang: natural 4 survives\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "aligned(N) may only RAISE — a weaker request is a no-op, not a lowering";
    EXPECT_FALSE(model.hasErrors());
}

// UNION: the channel is per-COMPOSITE, not per-struct. CLANG (MEASURED):
// `union U { char a; int b; } __attribute__((aligned(32)));` → sizeof 32,
// _Alignof 32 (natural 4 / 4). RED-ON-DISABLE: drop the seed in computeLayout's
// Union arm and this falls to 4 / 4.
TEST(SemanticAnalyzerC, CompositeAlignedOnUnionRaisesAlignAndSize) {
    auto cu = buildShippedUnit("c", {
        "union U { char a; int b; } __attribute__((aligned(32)));\n"
        "_Static_assert(sizeof(union U) == 32, \"clang: 32\");\n"
        "_Static_assert(_Alignof(union U) == 32, \"clang: 32\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// THE THREE-CHANNEL INTERACTION: composite `aligned(16)` + composite `packed` + a
// MEMBER `alignas(8)`. CLANG (MEASURED, compiled and run):
//     struct K { char a; _Alignas(8) int b; } __attribute__((packed, aligned(16)));
//         → sizeof 16, _Alignof 16
// Each channel does its own job and none overrides another: packed drops the
// per-field baseline to 1, the member alignas raises member `b` back to 8 (so b@8,
// extent 12), and the composite aligned(16) raises the aggregate — rounding 12 up
// to 16. `struct M` is the un-packed twin (same members, aligned(16), no packed) and
// clang gives it 16 / 16 as well — the member alignas(8) already pushes `b` to 8
// either way, so these two AGREE, which is the point: this pin is a clang-conformance
// check on the three-way composition, NOT a packed discriminator. The pin that
// isolates packed under a composite aligned is
// `CompositeScanAccumulatesAcrossAdjacentAttrSpecifiers` (6/2 vs 5/1 vs 8/4).
TEST(SemanticAnalyzerC, CompositeAlignedComposesWithPackedAndMemberAlignas) {
    auto cu = buildShippedUnit("c", {
        "struct K { char a; alignas(8) int b; }"
        " __attribute__((packed, aligned(16)));\n"
        "_Static_assert(sizeof(struct K) == 16, \"clang: 16\");\n"
        "_Static_assert(_Alignof(struct K) == 16, \"clang: 16\");\n"
        "struct M { char a; alignas(8) int b; } __attribute__((aligned(16)));\n"
        "_Static_assert(sizeof(struct M) == 16, \"clang: 16\");\n"
        "_Static_assert(_Alignof(struct M) == 16, \"clang: 16\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "packed (baseline 1), member alignas (raises one field) and composite "
           "aligned (raises the aggregate) are three independent channels";
    EXPECT_FALSE(model.hasErrors());
}

// The SHARED alignment ladder still applies to the composite position — an
// `aligned(3)` on a struct definition is not a power of two and fails loud with the
// SAME code `alignas(3)` uses, because it is literally the same `foldAlignmentOperand`
// call. RED-ON-DISABLE: route the composite clause around the shared ladder and this
// count drops to 0 (the struct would silently lay out un-aligned).
TEST(SemanticAnalyzerC, CompositeAlignedNonPowerOfTwoFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "struct B { char a; int b; } __attribute__((aligned(3)));\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNotPowerOfTwo), 1u)
        << "a composite aligned(3) must reject for the same reason and with the "
           "same code as alignas(3) — one ladder, not two";
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_AlignasNotPowerOfTwo) continue;
        EXPECT_NE(d.actual.find("aligned"), std::string::npos) << d.actual;
    }
}

// The BARE composite `__attribute__((aligned))` (no argument) means "the target's
// maximum useful alignment" in gcc — a target-dependent number this engine refuses
// to invent. It fails LOUD, exactly like the declaration-level bare form, rather
// than being silently treated as no request at all.
TEST(SemanticAnalyzerC, CompositeBareAlignedWithNoArgumentFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "struct C { char a; int b; } __attribute__((aligned));\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_UnknownTypeAttribute) continue;
        EXPECT_EQ(d.severity, DiagnosticSeverity::Error);
        EXPECT_NE(d.actual.find("explicit alignment argument"), std::string::npos)
            << d.actual;
    }
}

// ★★ THE MULTI-SURFACE COMPOSITE SCAN (D-CSUBSET-PACKED-AFTER-KEYWORD-POSITION).
// `scanCompositePacked` must be surface-COUNT-agnostic: it accumulates EVERY
// matching attribute surface on the specifier node rather than selecting the first
// and stopping. The `break` it used to carry is what BLOCKED the after-keyword slot
// (`struct __attribute__((packed)) S { … };`, the `mach-o/dyld_images.h` shape) —
// with it, a lead slot under a distinct rule name is invisible to the scan and
// `packed` is silently dropped, MEASURED as a clean compile at sizeof 8 against
// clang's 5.
//
// ★ WHAT THIS TEST PINS, AND WHAT IT DOES NOT. The shipped c grammar has
// exactly ONE composite attribute surface today (the trailing `compositeAttrList`)
// and declares no `declarationAttrSlotRules` on `structSpec`/`unionSpec`, so a
// two-ROOT tree is not constructible from C source yet — the after-keyword pin is
// WAITING on the config re-add and is NOT live here. What IS reachable, and is
// pinned below, is the same generalization one level in: TWO ADJACENT attribute
// SPECIFIERS under that one surface, contributing DIFFERENT facts. If the scan
// stops at the first attribute node, `aligned(16)` is lost and the size reverts.
//
// ★ THE NUMBERS ARE CHOSEN TO DISCRIMINATE. `aligned(2)` on a `{char, int}` is the
// one value where each attribute is independently observable — drop either and BOTH
// assertions move, in DIFFERENT directions. CLANG GROUND TRUTH (MEASURED, compiled
// and run on arm64 macOS, `-fsyntax-only -Wall -Wextra -isysroot $(xcrun
// --show-sdk-path)` clean):
//     struct V {char a; int b;} __attribute__((packed)) __attribute__((aligned(2)));
//         → sizeof 6, _Alignof 2     ← BOTH honored
//     ...with only `packed`                 → sizeof 5, _Alignof 1
//     ...with only `aligned(2)`             → sizeof 8, _Alignof 4
// Reversing the two specifiers (`struct W`) gives 6 / 2 as well: the scan is
// ORDER-independent, which is the other half of "no clause swallows another".
TEST(SemanticAnalyzerC, CompositeScanAccumulatesAcrossAdjacentAttrSpecifiers) {
    auto cu = buildShippedUnit("c", {
        "struct V { char a; int b; }"
        " __attribute__((packed)) __attribute__((aligned(2)));\n"
        "_Static_assert(sizeof(struct V) == 6, \"clang: 6 (both honored)\");\n"
        "_Static_assert(_Alignof(struct V) == 2, \"clang: 2 (both honored)\");\n"
        "struct W { char a; int b; }"
        " __attribute__((aligned(2))) __attribute__((packed));\n"
        "_Static_assert(sizeof(struct W) == 6, \"clang: order-independent\");\n"
        "_Static_assert(_Alignof(struct W) == 2, \"clang: order-independent\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "the composite scan must examine EVERY attribute specifier on the "
           "surface: keeping only the first gives 5/1 or 8/4, never clang's 6/2";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// The strict UNKNOWN-name gate on a composite is UNCHANGED by the new `aligned`
// arm — a typo'd GNU composite attribute still fails loud, and specifically a typo
// of `aligned` itself does NOT slip through the effect-row match. This is the
// control that proves the new arm keys on the DECLARED effect row, not on "the
// clause has an argument".
TEST(SemanticAnalyzerC, CompositeMisspelledAlignedStillFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "struct D { char a; int b; } __attribute__((alinged(16)));\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 1u)
        << "a typo'd composite attribute must not be silently unapplied";
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_UnknownTypeAttribute) continue;
        EXPECT_NE(d.actual.find("alinged"), std::string::npos) << d.actual;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// TF-C77 — D-CSUBSET-ATTRIBUTE-LEADING-WITH-STORAGE-CLASS, the SEMANTIC tier.
// The linkage-tier pins live in tests/hir/test_hir_lowering_c.cpp; these
// cover the ATTRIBUTE-SEMANTICS sink (alignment) and the BLOCK-SCOPE twin of the
// new mid-position slot.
// ════════════════════════════════════════════════════════════════════════════

// ★★ MODE 2 AT BLOCK SCOPE — the `varDecl` sibling, and the reason it was
// included rather than cut.
//
// MEASURED at the pre-change HEAD: `int __attribute__((aligned(16))) x = 1;`
// inside a function body was a hard P0009, while the byte-identical file-scope
// spelling was about to become legal. One attribute must not mean two different
// things depending on SCOPE, so `declAttrRun` joined BOTH branches of `varDecl`
// in the same commit as `topLevelDecl`.
//
// ★ ASSERTS THE APPLIED ALIGNMENT (32 — a genuine OVER-alignment for `int`,
// natural 4), not that it compiled. A slot that parses and drops the value emits
// NOTHING, so only the stored number can catch it.
//
// RED-ON-DISABLE: drop `declarationAttrSlotRules` from the varDecl row → the
// declaration still parses and this reads no value at all. Remove `declAttrRun`
// from the varDecl branches → P0009, and the ASSERT_NE on the record fires first.
TEST(SemanticAnalyzerC, MidPositionAlignedAppliesAtBlockScope) {
    auto cu = buildShippedUnit("c", {
        "int main(void){ int __attribute__((aligned(32))) x = 1; return x; }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    SymbolRecord const* x = findSym(model, "x");
    ASSERT_NE(x, nullptr) << "the block-scope mid-position form must PARSE";
    ASSERT_TRUE(x->explicitAlignment.has_value())
        << "the mid-position `aligned(32)` on a LOCAL must reach the same "
           "attribute-semantics sink the file-scope spelling reaches";
    EXPECT_EQ(*x->explicitAlignment, 32u);
}

// The block-scope MID position and the block-scope LEADING position must agree,
// on the same program. This is the scope-level twin of the file-scope symmetry
// pin in the HIR suite: an attribute that means one thing before the type and
// another after it is the exact defect class this cycle closes.
TEST(SemanticAnalyzerC, BlockScopeMidAndLeadingAlignedAgree) {
    for (char const* decl : {
             "__attribute__((aligned(32))) int x = 1;",     // leading
             "int __attribute__((aligned(32))) x = 1;"}) {  // mid (TF-C77)
        auto cu = buildShippedUnit("c", {
            std::string("int main(void){ ") + decl + " return x; }\n" });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
        SymbolRecord const* x = findSym(model, "x");
        ASSERT_NE(x, nullptr) << decl;
        ASSERT_TRUE(x->explicitAlignment.has_value()) << decl;
        EXPECT_EQ(*x->explicitAlignment, 32u) << decl;
    }
}

// MODE 2 anti-hijack at BLOCK scope: `declAttrRun` is a sibling of `declHead`,
// never a child, so an attribute identifier that also names a real type must not
// be resolved as the declaration's type. `long` vs `int` makes the hijack
// OBSERVABLE as a size — a check that `x` is merely "some integer" would pass
// through the very hijack this guards.
TEST(SemanticAnalyzerC, BlockScopeMidPositionDoesNotHijackTheHeadType) {
    auto cu = buildShippedUnit("c", {
        "typedef long aligned;\n"
        "int main(void){ int __attribute__((aligned(4))) x = 1; "
        "return (int)sizeof(x); }\n" });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_FALSE(model.hasErrors());
    SymbolRecord const* x = findSym(model, "x");
    ASSERT_NE(x, nullptr);
    ASSERT_TRUE(x->explicitAlignment.has_value())
        << "a typedef named `aligned` must not stop the attribute being honored";
    EXPECT_EQ(*x->explicitAlignment, 4u);
}

// ★★ MODE 1, THE NORETURN SINK — the pin that proves the after-keyword position
// is REACHED by the semantic specifier scan, not merely parsed.
//
// `specifierPrefixNamesNoreturn` walks the row's `specifierPrefixChild`. Mode 1
// works only because `externSpecifiers` IS that prefix and is MANDATORY (it owns
// the `extern` keyword), so it is always the first visible child and always
// returned. This test is what makes that "zero new wiring" claim VERIFIED rather
// than asserted — the Tcl `TCL_NORETURN` shape must MEAN noreturn, not just parse.
//
// ★ ASSERTS THE APPLIED BIT. The end-to-end consequence is witnessed separately
// and more strongly at the HIR verifier: MEASURED, `int pick(int c){ if (c)
// return 1; die(2); }` compiles with exit 0 when `die` is declared
// `extern __attribute__((__noreturn__)) void die(int);` and fails
// `H0003 non-void function may fall through without returning a value` when the
// attribute is removed — so the flag really does terminate the path.
//
// RED-ON-DISABLE: remove `attrSpec` from `externSpecifiers` → P0009 (the shape
// stops parsing). Remove `noreturn` from `semantics.noreturnAttributeNames` →
// this parses clean and the bit reads false, silently.
TEST(SemanticAnalyzerC, ExternHeadNoreturnAttributeIsApplied) {
    auto model = analyzeShipped("c", {
        "extern __attribute__((__noreturn__)) void die(int);\n"
        "int main(void){ die(1); return 0; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_TRUE(survivingFnIsNoreturn(model, "die"))
        << "an attribute written AFTER the `extern` keyword must reach the "
           "noreturn scan — the dunder spelling `__noreturn__` normalizes to "
           "`noreturn`, exactly as it does in the leading position";
}

// ★★ A DEFECT THIS CYCLE FOUND AND DELIBERATELY DID NOT FIX — RECORDED HERE SO
// IT IS GREPPABLE INSTEAD OF INVISIBLE.
//
// This started life as a symmetry pin ("the after-keyword and after-declarator
// positions must agree on `noreturn`") and it FAILED — which is the useful
// outcome. MEASURED: the AFTER-DECLARATOR spelling does NOT reach the noreturn
// sink at all. End to end, on the identical program:
//
//   extern __attribute__((__noreturn__)) void die(int);   (mode 1, NEW)  exit 0
//   extern void die(int) __attribute__((__noreturn__));   (trailing)     H0003
//       "non-void function may fall through without returning a value"
//
// CAUSE: `specifierPrefixNamesNoreturn` scans the row's `specifierPrefixChild`
// ONLY. Mode 1 works because `externSpecifiers` IS that prefix. TF-C73 made the
// after-declarator run a LINKAGE scan root but never a NORETURN one, so the
// trailing spelling is parsed, linkage-folded, and its noreturn meaning dropped.
//
// WHY IT IS NOT FIXED HERE: it is PRE-EXISTING (nothing in TF-C77 touches that
// scan or that run), it is a SAFE MISS rather than a miscompile — dropping the
// flag can only produce a SPURIOUS diagnostic, never wrong code, which is the
// posture `specifierPrefixNamesNoreturn`'s own comment states — and closing it
// means deciding whether a per-DECLARATOR trailing run may confer a
// DECLARATION-level property, which is a different anchor's question.
//
// ★ THE EXPECT_FALSE BELOW ASSERTS A BUG, NOT A DESIRED PROPERTY. When the
// trailing position is wired to the noreturn scan, this test WILL fail — that is
// the point. Flip it to EXPECT_TRUE then, and fold both arms back into one
// symmetry loop. Do NOT "fix" it by deleting the assertion.
TEST(SemanticAnalyzerC, ExternTrailingNoreturnIsNotYetHonoredKnownGap) {
    auto head = analyzeShipped("c", {
        "extern __attribute__((__noreturn__)) void die(int);\n"
        "int main(void){ die(1); return 0; }\n",
    });
    EXPECT_FALSE(head.hasErrors());
    EXPECT_TRUE(survivingFnIsNoreturn(head, "die"))
        << "mode 1 (after-keyword) IS honored — this half is the new behavior";

    auto trail = analyzeShipped("c", {
        "extern void die(int) __attribute__((__noreturn__));\n"
        "int main(void){ die(1); return 0; }\n",
    });
    EXPECT_FALSE(trail.hasErrors())
        << "the trailing form still PARSES and links cleanly — the gap is the "
           "dropped semantic fact, not a rejection";
    EXPECT_FALSE(survivingFnIsNoreturn(trail, "die"))
        << "KNOWN GAP (see banner): if this now reads TRUE the trailing position "
           "has been wired to the noreturn scan — flip this to EXPECT_TRUE and "
           "restore the symmetry loop";
}

// ─────────────────────────────────────────────────────────────────────────────
// TF-C88 — GNU/Clang ASM LABEL (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME,
// GCC 6.47.5) and TYPEDEF MULTI-DECLARATOR
// (D-CSUBSET-TYPEDEF-MULTI-DECLARATOR).
//
// The two defects behind 41 of the 49 macho-leg corpus errors at HEAD 2637623.
// Every assertion below is on an OBSERVABLE PROPERTY (the recorded assembler
// name, the resolved type of each alias), never on "it compiles clean" — a
// parse-and-ignore asm label and a dropped 2nd typedef alias BOTH compile clean,
// which is exactly what makes them dangerous.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
// Every symbol spelled `name`, oldest first — a multi-declarator declaration and
// a proto+definition merge both produce several records for one name, and the
// distinction between them is load-bearing in the merge pin below.
[[nodiscard]] std::vector<SymbolRecord const*>
allSymbolsNamed(SemanticModel const& model, std::string_view name) {
    std::vector<SymbolRecord const*> out;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == name) out.push_back(&model.symbols()[i]);
    }
    return out;
}
// The asm label recorded on the FIRST symbol spelled `name` that carries one —
// the merge deliberately propagates the label onto the SURVIVOR, so a proto+def
// pair must report it whichever record the scan reaches first.
[[nodiscard]] std::string asmNameOf(SemanticModel const& model,
                                    std::string_view name) {
    for (auto const* r : allSymbolsNamed(model, name))
        if (!r->asmName.empty()) return r->asmName;
    return {};
}
} // namespace

// FORM 1 — a label on an OBJECT at file scope. The MEASURED clang behavior is
// that the string is the symbol VERBATIM; the assertion is therefore on the
// exact bytes, `myglobal` and not `_myglobal`, because mangling the label on top
// is the plausible wrong implementation and it compiles just as cleanly.
// RED-ON-DISABLE: delete the `rec.asmName = ...` write in Pass-1's declarator
// loop and this reads "" while the program still compiles.
TEST(SemanticAnalyzerC, AsmLabelOnFileScopeObjectIsRecordedVerbatim) {
    auto m = analyzeShipped("c", {
        "int gv __asm(\"myglobal\") = 7;\n"
        "int main(void) { return gv; }\n",
    });
    EXPECT_FALSE(m.diagnostics().hasErrors());
    EXPECT_EQ(asmNameOf(m, "gv"), "myglobal");
}

// FORM 2 — a label on a FUNCTION DEFINITION (declarator + body in one).
//
// ★ A DELIBERATE, DOCUMENTED DIVERGENCE FROM CLANG, and the reason is a defect
// this cycle also closed. `/usr/bin/clang` REJECTS this spelling ("expected ';'
// after top level declarator") purely for GCC compatibility — GCC wants the label
// on a preceding DECLARATION. DSS honors it, because the rename it asks for is
// unambiguous and the symbol it names is right there. That is over-permissiveness,
// never a silent drop: the label IS applied (asserted below), so nothing the
// programmer wrote is ignored.
//
// The same edit is what makes `int f(void) __attribute__((noinline)) { … }`
// compile. MEASURED at the pre-TF-C88 HEAD that shape was REFUSED as "a function
// definition cannot carry an initializer" — a diagnostic naming a construct the
// source does not contain — because the definition gate counted every Internal
// child past the declarator and had never received TF-C62's decoration skip. It
// was the sixth init-detection site and the one left behind.
TEST(SemanticAnalyzerC, AsmLabelOnFunctionDefinitionIsRecorded) {
    auto m = analyzeShipped("c", {
        "int fn(int x) __asm(\"myfn\") { return x; }\n"
        "int main(void) { return fn(42); }\n",
    });
    EXPECT_FALSE(m.diagnostics().hasErrors());
    EXPECT_EQ(asmNameOf(m, "fn"), "myfn");

}

// TF-C88 — the PRE-EXISTING defect this cycle fixed in passing, with its OWN
// guard so it has its own red-on-disable arm rather than riding an asm-label
// test's coat-tails.
//
// THE DEFECT (MEASURED at 2637623, before any TF-C88 change): the function-
// DEFINITION gate in `pass2Post` open-coded its own init detection as "count the
// Internal children of the init-declarator; more than one means an initializer is
// present". That counts the DECLARATOR itself as the one, so ANY second Internal
// child trips it — including an after-declarator ATTRIBUTE. So
// `int f(void) __attribute__((noinline)) { return 1; }` was refused
// `S_InvalidFunctionDeclarator: a function definition cannot carry an
// initializer` — a diagnostic naming a construct the source does not contain,
// on code `/usr/bin/clang` compiles (it emits only a -Wgcc-compat warning that
// GCC would not allow `noinline` in that position).
//
// It was the SIXTH of six init-detection scans in the SEMANTIC tier and the ONLY
// one of the six TF-C62 never reached — the other five (declaratorHasInitializer,
// initializerNodeOf, the constexpr scan, the auto-inference scan, the Pass-2
// init-type scan) all received the attribute skip then. It is fixed HERE, not in
// its own cycle, because the asm label would have inherited the identical
// mis-read: TF-C87's lesson is that a defect fixed in one form and left live in
// another is the expensive kind.
//
// RED-ON-DISABLE: restore the `internals > 1` count in place of the
// decoration-aware `initializers > 0` and this test reds while every asm-label
// test stays green (verified against the final build).
TEST(SemanticAnalyzerC, AfterDeclaratorDecorationOnDefinitionIsNotAnInitializer) {
    // The ATTRIBUTE form — the one that predates this cycle entirely.
    auto attr = analyzeShipped("c", {
        "int g(void) __attribute__((noinline)) { return 1; }\n"
        "int main(void) { return g(); }\n",
    });
    EXPECT_FALSE(attr.diagnostics().hasErrors())
        << "an after-declarator attribute on a DEFINITION is not an initializer";
    EXPECT_EQ(countCode(attr.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u);

    // A REAL initializer on a definition must STILL fail loud — the fix narrows
    // the count, it does not remove the gate. Without this the arm above could be
    // satisfied by deleting the check outright.
    auto real = analyzeShipped("c", {
        "int h(void) = 5 { return 1; }\n"
        "int main(void) { return h(); }\n",
    });
    EXPECT_TRUE(real.diagnostics().hasErrors())
        << "a genuine initializer on a function definition must stay rejected";
}

// FORM 3 — a label on a PROTOTYPE renames the later DEFINITION. This is the
// shape every `__DARWIN_ALIAS` header actually uses (the header declares the
// rename; the definition lives elsewhere), and MEASURED against clang the
// emitted symbol is the label.
//
// ★ THE ASSERTION IS ON THE SURVIVING RECORD, NOT "some record named deffn".
// The lax form was written first and the RED-ON-DISABLE battery caught it: with
// the merge carry deleted, the ABSORBED proto still holds the label, so an
// any-record scan reports "mydeffn" and the test passes while the DEFINITION —
// the record `nameOf` actually reads — emits `_deffn`. The survivor is the record
// with `isAbsorbedProto == false`; asserting there is the only form that can see
// the carry at all.
TEST(SemanticAnalyzerC, AsmLabelOnPrototypeRidesTheRedeclarationMerge) {
    auto m = analyzeShipped("c", {
        "int deffn(int) __asm(\"mydeffn\");\n"
        "int deffn(int x) { return x; }\n"
        "int main(void) { return deffn(1); }\n",
    });
    EXPECT_FALSE(m.diagnostics().hasErrors());
    auto const recs = allSymbolsNamed(m, "deffn");
    ASSERT_EQ(recs.size(), 2u) << "a proto + its definition mint two records";
    SymbolRecord const* survivor = nullptr;
    for (auto const* r : recs)
        if (!r->isAbsorbedProto) survivor = r;
    ASSERT_NE(survivor, nullptr) << "exactly one record must survive the merge";
    EXPECT_EQ(survivor->asmName, "mydeffn")
        << "the label must reach the SURVIVING (defining) record — that is the "
           "one `nameOf` names the emitted symbol from";
}

// The mirror direction: the DEFINITION comes first and the labelled redundant
// declaration follows. The survivor is the prior record, so the carry runs down
// the other branch of the merge — a branch that would otherwise never be
// exercised and would drift silently.
TEST(SemanticAnalyzerC, AsmLabelOnTrailingRedeclarationRidesTheMerge) {
    auto m = analyzeShipped("c", {
        "int deffn(int x) { return x; }\n"
        "int deffn(int) __asm(\"mydeffn\");\n"
        "int main(void) { return deffn(1); }\n",
    });
    EXPECT_FALSE(m.diagnostics().hasErrors());
    auto const recs = allSymbolsNamed(m, "deffn");
    ASSERT_EQ(recs.size(), 2u);
    SymbolRecord const* survivor = nullptr;
    for (auto const* r : recs)
        if (!r->isAbsorbedProto) survivor = r;
    ASSERT_NE(survivor, nullptr);
    EXPECT_EQ(survivor->asmName, "mydeffn");
}

// FORM 4 — a label on an `extern` declaration (the import rail's entry point).
TEST(SemanticAnalyzerC, AsmLabelOnExternDeclarationIsRecorded) {
    auto m = analyzeShipped("c", {
        "extern int extfn(int) __asm(\"_myextfn\");\n"
        "int main(void) { return extfn(1); }\n",
    });
    EXPECT_FALSE(m.diagnostics().hasErrors());
    EXPECT_EQ(asmNameOf(m, "extfn"), "_myextfn");
}

// FORM 5 — the ADJACENT-CONCATENATED payload, which is the ONLY spelling the
// macOS SDK ever produces (`__asm("_" __STRING(sym) __DARWIN_SUF_UNIX03)`).
// A consumer that read only the first body would record "_" and rename every
// SDK symbol to a single underscore — clean-compiling and catastrophic. The
// assertion is on the JOINED bytes, which is what makes it catch that.
TEST(SemanticAnalyzerC, AsmLabelConcatenatesAdjacentStringPieces) {
    auto m = analyzeShipped("c", {
        "#define __STRING(x) #x\n"
        "#define SUF \"$UNIX2003\"\n"
        "#define ALIAS(sym) __asm(\"_\" __STRING(sym) SUF)\n"
        "int aliased(void) ALIAS(aliased);\n"
        "int main(void) { return 42; }\n",
    });
    EXPECT_FALSE(m.diagnostics().hasErrors()) << "the SDK spelling must parse";
    EXPECT_EQ(asmNameOf(m, "aliased"), "_aliased$UNIX2003");
}

// FORM 6 — PER-DECLARATOR granularity. Two labels in ONE declaration must land
// on their OWN symbols; a declaration-level read (the plausible wrong
// implementation, and the shape `libraryOverride` legitimately uses) would give
// both the same name and silently alias two symbols onto one.
TEST(SemanticAnalyzerC, AsmLabelsAreRecordedPerDeclarator) {
    auto m = analyzeShipped("c", {
        "int a __asm(\"aa\") = 1, b __asm(\"bb\") = 2, plain = 3;\n"
        "int main(void) { return a + b + plain; }\n",
    });
    EXPECT_FALSE(m.diagnostics().hasErrors());
    EXPECT_EQ(asmNameOf(m, "a"), "aa");
    EXPECT_EQ(asmNameOf(m, "b"), "bb");
    EXPECT_EQ(asmNameOf(m, "plain"), "")
        << "an undecorated declarator in the same declaration must stay unlabelled";
}

// FORM 7 — a label INTERLEAVED with an attribute, both orders. The run is a
// `{repeat alt}` precisely so neither order needs a rule, and both must keep the
// initializer detection correct (the label is not the init value).
TEST(SemanticAnalyzerC, AsmLabelInterleavesWithAttributesEitherOrder) {
    auto first = analyzeShipped("c", {
        "int p __asm(\"pp\") __attribute__((aligned(8))) = 1;\n"
        "int main(void) { return p; }\n",
    });
    EXPECT_FALSE(first.diagnostics().hasErrors()) << "label before attribute";
    EXPECT_EQ(asmNameOf(first, "p"), "pp");
    auto second = analyzeShipped("c", {
        "int q __attribute__((aligned(8))) __asm(\"qq\") = 1;\n"
        "int main(void) { return q; }\n",
    });
    EXPECT_FALSE(second.diagnostics().hasErrors()) << "attribute before label";
    EXPECT_EQ(asmNameOf(second, "q"), "qq");
}

// FAIL-LOUD 1 — an EMPTY label. The most dangerous invalid form: an empty name
// reaches `nameOf` as the module-private signal, the symbol-table row is
// dropped, and the writer substitutes a synthetic `sym_<id>`. RED-ON-DISABLE:
// remove the `decoded->empty()` arm in `readAsmLabel` and this compiles clean.
TEST(SemanticAnalyzerC, EmptyAsmLabelFailsLoud) {
    auto m = analyzeShipped("c", {
        "int f(void) __asm(\"\");\n"
        "int main(void) { return 42; }\n",
    });
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AsmLabelInvalid), 1u);
}

// FAIL-LOUD 2 — TWO labels on one declarator. Never first-wins: which assembler
// name was intended is genuinely unknown.
TEST(SemanticAnalyzerC, DuplicateAsmLabelFailsLoud) {
    auto m = analyzeShipped("c", {
        "int f(void) __asm(\"a\") __asm(\"b\");\n"
        "int main(void) { return 42; }\n",
    });
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AsmLabelDuplicate), 1u);
    EXPECT_EQ(asmNameOf(m, "f"), "")
        << "a rejected label must not leave a half-applied rename behind";
}

// FAIL-LOUD 3 — a label on an AUTOMATIC local. WARNS and drops (matching clang,
// MEASURED: "ignored asm label 'mylocal' on automatic variable"). Erroring would
// refuse C every toolchain accepts; silence would leave the programmer believing
// a rename happened. The `asmName` assertion is what proves the DROP actually
// happened rather than the warning being cosmetic.
TEST(SemanticAnalyzerC, AsmLabelOnAutomaticLocalWarnsAndIsDropped) {
    auto m = analyzeShipped("c", {
        "int main(void) { int x __asm(\"mylocal\") = 1; return x; }\n",
    });
    EXPECT_FALSE(m.diagnostics().hasErrors()) << "a warning, never an error";
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_AsmLabelOnAutomaticVariable), 1u);
    EXPECT_EQ(asmNameOf(m, "x"), "");
}

// The COMPLEMENT of the warning above: a `static` local DOES have a symbol, so
// its label is honored. Without this pin the warning could be widened to every
// block-scope declaration and nothing would notice.
TEST(SemanticAnalyzerC, AsmLabelOnStaticLocalIsHonored) {
    auto m = analyzeShipped("c", {
        "int main(void) { static int s __asm(\"mystatic\") = 1; return s; }\n",
    });
    EXPECT_FALSE(m.diagnostics().hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_AsmLabelOnAutomaticVariable), 0u);
    EXPECT_EQ(asmNameOf(m, "s"), "mystatic");
}

// ── D-CSUBSET-TYPEDEF-MULTI-DECLARATOR ──────────────────────────────────────

// THE corpus witness, verbatim from the macOS SDK (`mach/vm_types.h`).
// Asserting all THREE aliases resolve to the SAME type is what catches the
// "lowered only the first declarator" implementation — which compiles clean and
// leaves aliases 2..N undeclared, so a `vm_map_read_t x;` later reads as an
// implicit-int or an undeclared type depending on the tier that trips first.
TEST(SemanticAnalyzerC, TypedefMultiDeclaratorBindsEveryAlias) {
    auto m = analyzeShipped("c", {
        "typedef unsigned int mach_port_t;\n"
        "typedef mach_port_t vm_map_t, vm_map_read_t, vm_map_inspect_t;\n"
        "vm_map_t a; vm_map_read_t b; vm_map_inspect_t c;\n"
        "int main(void) { return (int)(a + b + c); }\n",
    });
    EXPECT_FALSE(m.diagnostics().hasErrors()) << "SDK mach/vm_types.h";
    auto const* base = findSymbolNamed(m, "mach_port_t");
    ASSERT_NE(base, nullptr);
    for (char const* alias : {"vm_map_t", "vm_map_read_t", "vm_map_inspect_t"}) {
        auto const* r = findSymbolNamed(m, alias);
        ASSERT_NE(r, nullptr) << alias << " was not bound";
        EXPECT_EQ(r->kind, DeclarationKind::Type) << alias;
        EXPECT_EQ(r->type.v, base->type.v)
            << alias << " must alias the SAME type as the shared head";
    }
}

// PER-SLOT declarator suffixes. A shared-head implementation (one type for the
// whole declaration) would type all three identically and compile clean — this
// is the assertion that distinguishes a real per-declarator fold from a
// copy-the-head one. `F` is a FUNCTION type, `P` a pointer, `A` an array.
TEST(SemanticAnalyzerC, TypedefMultiDeclaratorFoldsEachSuffixSeparately) {
    auto m = analyzeShipped("c", {
        "typedef int *P, A[4], F(void);\n"
        "int main(void) { return 42; }\n",
    });
    EXPECT_FALSE(m.diagnostics().hasErrors());
    TypeInterner const& in = m.lattice().interner();
    auto const* p = findSymbolNamed(m, "P");
    auto const* a = findSymbolNamed(m, "A");
    auto const* f = findSymbolNamed(m, "F");
    ASSERT_NE(p, nullptr); ASSERT_NE(a, nullptr); ASSERT_NE(f, nullptr);
    EXPECT_EQ(in.kind(p->type), TypeKind::Ptr)   << "P must be int*";
    EXPECT_EQ(in.kind(a->type), TypeKind::Array) << "A must be int[4]";
    EXPECT_EQ(in.kind(f->type), TypeKind::FnSig) << "F must be int(void)";
}

// The single-declarator form must be UNCHANGED — the list rule replaces the bare
// declarator 1:1 at the same role index, so nothing about a one-alias typedef
// moves. Without this the index flip could regress every existing typedef and
// only the multi-declarator tests would notice.
TEST(SemanticAnalyzerC, TypedefSingleDeclaratorIsUnchanged) {
    auto m = analyzeShipped("c", {
        "typedef unsigned long long u_int64_t;\n"
        "typedef u_int64_t au_asflgs_t __attribute__ ((aligned(8)));\n"
        "au_asflgs_t v;\n"
        "int main(void) { return (int)v; }\n",
    });
    EXPECT_FALSE(m.diagnostics().hasErrors());
    auto const* base = findSymbolNamed(m, "u_int64_t");
    auto const* al   = findSymbolNamed(m, "au_asflgs_t");
    ASSERT_NE(base, nullptr); ASSERT_NE(al, nullptr);
    EXPECT_EQ(al->type.v, base->type.v);
}

// A BLOCK-SCOPE multi-declarator typedef — the statement position, which needs
// its own Block wrapper because a statement holds exactly one HIR node. Both
// aliases must bind IN the block.
TEST(SemanticAnalyzerC, BlockScopeTypedefMultiDeclaratorBindsEveryAlias) {
    auto m = analyzeShipped("c", {
        "int main(void) { typedef int LA, LB; LA a = 1; LB b = 2; return a + b; }\n",
    });
    EXPECT_FALSE(m.diagnostics().hasErrors());
    for (char const* alias : {"LA", "LB"}) {
        auto const* r = findSymbolNamed(m, alias);
        ASSERT_NE(r, nullptr) << alias << " was not bound in the block";
        EXPECT_EQ(r->kind, DeclarationKind::Type) << alias;
    }
}

// An ABSTRACT slot in the middle of the list fails LOUD per slot — `typedef int
// A, *, B;` declares nothing at slot 2. Silently binding A and B and skipping
// the middle would declare fewer aliases than were written.
TEST(SemanticAnalyzerC, TypedefMultiDeclaratorAbstractSlotFailsLoud) {
    auto m = analyzeShipped("c", {
        "typedef int A, *, B;\n"
        "int main(void) { return 42; }\n",
    });
    EXPECT_GE(countCode(m.diagnostics(),
                        DiagnosticCode::S_DeclarationDeclaresNothing), 1u);
}

// ── TF-C89 — THE SHIPPED-TYPEDEF DEAD WINDOW ─────────────────────────────────
// D-CSUBSET-SHIPPED-TYPEDEF-POSITION-BLIND-SUPPRESSION
//
// MEASURED on the SQLite corpus (arm64 macho, per-TU isolated census at
// fc0908d, per-code cap temporarily lifted in a local build because the shipped
// cap of 50 was saturated on two TUs): 80 of the leg's 123 `S0006` came from ONE
// mechanism — MEASURED after the fix, S0006 123 -> 43. The
// shipped-descriptor TYPEDEF injection's goal-2 skip ("a user declaration of
// the name wins") keyed on `userDeclaredNames` — a WHOLE-TU, POSITION-BLIND
// name set. The user's own typedef, by contrast, is POSITION-SENSITIVE: Pass
// 1.5 resolves declaration types in TREE ORDER, so its `SymbolRecord::type` is
// still InvalidType while EARLIER declarations resolve. One user typedef
// anywhere in the TU therefore DELETED the shipped typedef, leaving a DEAD
// WINDOW from the top of the TU down to the user's redeclaration point in
// which the name resolved to NOTHING — no shipped symbol (skipped) and no user
// symbol (not yet typed). C 6.2.1p7 puts the ENCLOSING declaration in scope for
// exactly that region.
//
// SQLite hits it because `sqliteInt.h` USES the names early
// (`#define INT16_TYPE int16_t` + `typedef INT16_TYPE i16;` / `LogEst`,
// `typedef uintptr_t uptr;`) and the platform's OWN `typedef short int16_t;`
// (the macOS SDK's `<sys/_types/_int16_t.h>`, pulled in later by
// `<malloc/malloc.h>` / `<sys/sysctl.h>`) lands LATER in the same TU. i16 / i8 /
// LogEst / ynVar all died with the deleted shipped typedef.
//
// The fix has TWO arms and NEITHER works alone:
//   A) the typedef injection's goal-2 skip keys on `userNonTypeDeclaredNames` —
//      a user TYPEDEF of the name no longer deletes the shipped one (a user
//      OBJECT/FUNCTION claim still does);
//   B) `resolveTypeNodeImpl`'s alias arm walks with `ScopeTree::lookupIf`, so a
//      binding that is not usable as a type alias (not Type-kind, or Type-kind
//      with an unresolved type) is SKIPPED and the walk CONTINUES OUTWARD.
// Every pin below names which arm(s) turn it red.

namespace {

// Build + analyze `mainSrc` against the REAL src/dss-config/shippedLibs — the
// same descriptors the production driver ships — so a regression in stdint.json
// itself flips the pin red rather than being mirrored green by a scratch copy.
[[nodiscard]] SemanticModel analyzeWithRealShippedLibs(std::string mainSrc) {
    fs::path const shipped = findRealShippedLibsDir();   // throws if unresolvable
    auto schema = loadShippedSchema("c");
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addSystemDir(shipped);
    builder.addInMemory(std::move(mainSrc), "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault());
}

} // namespace

// FORM 1 — THE CORPUS FORM, against the REAL shipped <stdint.h>. Byte-for-byte
// the SQLite shape: use `int16_t` in a typedef ABOVE the platform's own
// `typedef short int16_t;`, then use the alias. RED on arm A ALONE (the shipped
// int16_t is deleted, so the alias's base is unknown) and RED on arm B ALONE
// (the not-yet-typed user binding stops the walk before the shipped one).
//
// The assertion is TWO-SIDED: `alias16` must be exactly I16. "No S_UnknownType"
// alone would pass on a resolution that silently picked some other width.
TEST(SemanticAnalyzerC, TFC89ShippedTypedefUsedAboveUserRedeclaration) {
    auto model = analyzeWithRealShippedLibs(
        "#include <stdint.h>\n"
        "typedef int16_t alias16;\n"   // the USE, ABOVE the redeclaration
        "typedef short int16_t;\n"     // the platform's own, C11 6.7p3 legal
        "int main(void) { alias16 a = 1; return (int)a; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u)
        << "int16_t is in scope above the redeclaration (C 6.2.1p7) — the "
           "shipped typedef must not be deleted by a LATER user redeclaration";
    auto const* alias = findSymbolNamed(model, "alias16");
    ASSERT_NE(alias, nullptr);
    ASSERT_TRUE(alias->type.valid());
    EXPECT_EQ(model.lattice().interner().kind(alias->type), TypeKind::I16)
        << "alias16 must be exactly the 16-bit core the shipped int16_t names";
}

// FORM 2 — ARM A's OWN pin, on the SURVIVING INJECTION rather than on
// resolution. With a user typedef of the same name present, BOTH declarations
// must exist (the injected one in the CU root, the user's in the tree root, one
// shadowing the other) — and, because they live in DIFFERENT scopes, the Pass-1
// same-scope collision check must NOT fire. RED on arm A alone: the count drops
// to 1. Deliberately independent of arm B, which cannot move either number.
TEST(SemanticAnalyzerC, TFC89ShippedTypedefSurvivesUserTypedefOfSameName) {
    ScratchDir sysDir{Location::Temp, "tfc89-survive"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "my16.json",
        R"({ "header": "my16.h",
             "typedefs": [ { "name": "my16_t", "type": "i16" } ] })",
        "#include <my16.h>\n"
        "typedef short my16_t;\n"
        "int main(void) { my16_t v = 1; return (int)v; }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countSymbolsNamed(model, "my16_t"), 2u)
        << "the shipped typedef is injected in the CU root AND the user's is "
           "bound in the tree root — a user TYPEDEF redeclaration must not "
           "delete the shipped declaration";
    EXPECT_FALSE(hasCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol))
        << "the two bindings are in DIFFERENT scopes — no same-scope collision";
}

// FORM 3 — ARM B's OWN pin, with NO shipped descriptor anywhere in the CU, so
// arm A cannot possibly influence it. A BLOCK-SCOPE redeclaration of a
// file-scope typedef, USED above the redeclaration: C 6.2.1p7 scopes the inner
// `T` only after its declarator completes, so `typedef T U;` above it must see
// the FILE-SCOPE `int`. RED on arm B alone (the not-yet-typed inner binding
// stops the walk); untouched by arm A.
//
// TWO-SIDED on purpose: `U` must be I32 (the OUTER int), not I16 (the inner
// short). A "no diagnostics" pin would accept the wrong declaration winning.
TEST(SemanticAnalyzerC, TFC89InnerRedeclarationDoesNotHideOuterTypedef) {
    auto model = analyzeShipped("c", {
        "typedef int T;\n"
        "int main(void) {\n"
        "    typedef T U;\n"       // must resolve to the FILE-SCOPE T (int)
        "    typedef short T;\n"   // the inner redeclaration, LATER
        "    U u = 1;\n"
        "    return (int)u;\n"
        "}\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u)
        << "the file-scope T is in scope above the block-scope redeclaration";
    auto const* u = findSymbolNamed(model, "U");
    ASSERT_NE(u, nullptr);
    ASSERT_TRUE(u->type.valid());
    EXPECT_EQ(model.lattice().interner().kind(u->type), TypeKind::I32)
        << "U aliases the OUTER `typedef int T`, not the inner `short`";
}

// THE GOAL-2 GUARD (arm A must stay SELECTIVE, not become "always inject"). A
// user declaration that is NOT a type — an OBJECT of the shipped typedef's
// spelling — still WINS: the shipped typedef stays skipped, so the name in TYPE
// position keeps failing LOUD and exactly one symbol carries the name. RED if
// arm A is widened to drop the skip unconditionally.
TEST(SemanticAnalyzerC, TFC89NonTypeUserDeclStillSuppressesShippedTypedef) {
    ScratchDir sysDir{Location::Temp, "tfc89-goal2"};
    auto cu = buildAngleDescriptorUnit(
        sysDir, "myint.json",
        R"({ "header": "myint.h",
             "typedefs": [ { "name": "my_int_t", "type": "i32" } ] })",
        "#include <myint.h>\n"
        "int my_int_t;\n"                       // a user OBJECT of that name
        "int main(void) { my_int_t = 5; return my_int_t; }\n");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countSymbolsNamed(model, "my_int_t"), 1u)
        << "a user OBJECT claim keeps the goal-2 skip: the descriptor typedef "
           "must NOT be injected alongside it";
    auto const* obj = findSymbolNamed(model, "my_int_t");
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->kind, DeclarationKind::Variable)
        << "the surviving symbol is the user's object, not a Type";
}

// THE FAIL-LOUD GUARD for arm B: widening WHERE a type may be found must never
// widen WHETHER a miss is reported. A type name with NO usable binding on the
// whole chain still fails loud — arm B must not invent a fallback.
TEST(SemanticAnalyzerC, TFC89UnresolvableTypeNameStillFailsLoud) {
    auto model = analyzeShipped("c", {
        "typedef NoSuchTypeName Alias;\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_GE(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 1u)
        << "an unresolvable type name must still fail loud after the walk "
           "runs off the end of the scope chain";
}

// ── D-CSUBSET-PARAM-FN-TYPE-ADJUSTMENT (C 6.7.6.3p8) ─────────────────────────
//
// "A declaration of a parameter as 'function returning type' shall be adjusted
// to 'pointer to function returning type'." Before this, such a parameter was
// bound with a FnSig type and fell into the FnSig-typed-Variable arm of the
// Pass-1.5 bind → S0018 S_InvalidFunctionDeclarator ("function prototype
// declarations are not supported here"). MEASURED as 6 of sqlite's arm64/macho
// residuals, all in `src/mem1.c`, all through Darwin's FUNCTION typedefs
// ($SDK/usr/include/malloc/malloc.h — `memory_reader_t`,
// `vm_range_recorder_t`, `print_task_printer_t` are function, NOT
// function-pointer, typedefs, used bare as parameters).
//
// ★ THE FIX IS THE ADJUSTMENT, NOT THE SUPPRESSION. Silencing S0018 at the
// reject site would leave the symbol typed FnSig — a function VALUE in a
// parameter slot, i.e. a silent miscompile. The rule lives in
// `adjustParamDeclaredType`, the ONE chokepoint every parameter resolution
// funnels through (the definitive Pass-1.5 bind and all three
// `declRowDeclaredType` arms that `collectParamTypes` uses to build a FnSig).
//
// ★ THIS IS A MULTI-SITE CONTRACT AND THE SITES ARE COVERED SEPARATELY. The
// p7 array twin's corpus exercises only named params in definitions; that
// weakness is deliberately NOT reproduced. Every form below is its own test:
// inline, via typedef, prototype-vs-definition equality, ABSTRACT/type-only,
// nested inside a fn-pointer declarator (the real SDK shape), and a CALL that
// actually dispatches through the adjusted parameter.
//
// RED-ON-DISABLE, shared by every test in this block: delete the FnSig arm of
// `adjustParamDeclaredType` and each `S_InvalidFunctionDeclarator == 0` pin
// goes red (S0018 returns), plus every Ptr/FnSig structural pin flips to a bare
// FnSig.

namespace {
// The pointee FnSig of a parameter that C 6.7.6.3p8 adjusted, or InvalidType.
// ASSERTing through this keeps each pin one line and makes a bare-FnSig
// regression (the un-adjusted state) impossible to pass by accident.
[[nodiscard]] TypeId adjustedFnParamPointee(SemanticModel const& m, TypeId t) {
    TypeInterner const& in = m.lattice().interner();
    if (!t.valid() || in.kind(t) != TypeKind::Ptr) return InvalidType;
    auto const ops = in.operands(t);
    if (ops.empty() || in.kind(ops[0]) != TypeKind::FnSig) return InvalidType;
    return ops[0];
}
}  // namespace

// FORM (1) — a DEFINITION with a NAMED, INLINE function-typed parameter.
// `int g(int)` in parameter position is a function type, adjusted to
// `int (*)(int)`. Also the SIZE pin: the parameter IS a pointer, so its layout
// is pointer-sized (a FnSig has no layout at all — computeLayout nullopts —
// so this pin is only satisfiable by the adjusted type).
TEST(SemanticAnalyzerC, InlineFunctionTypedParamAdjustsToPointerToFunction) {
    auto cu = buildShippedUnit("c", {
        "int f(int g(int), int v) { return g(v); }\n"
        "int main(void) { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u)
        << "a function-typed PARAMETER is not a prototype — C 6.7.6.3p8 adjusts "
           "it to a pointer; S0018 here is the un-adjusted regression";
    EXPECT_FALSE(model.hasErrors());
    auto const* g = findSym(model, "g");
    ASSERT_NE(g, nullptr) << "the parameter symbol must still be bound";
    TypeId const pointee = adjustedFnParamPointee(model, g->type);
    ASSERT_TRUE(pointee.valid())
        << "the bound parameter must be Ptr<FnSig>, never a bare FnSig (a "
           "function VALUE in a parameter slot is the silent miscompile)";
    TypeInterner const& in = model.lattice().interner();
    auto const params = in.fnParams(pointee);
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(in.kind(params[0]), TypeKind::I32);
    EXPECT_EQ(in.kind(in.fnResult(pointee)), TypeKind::I32);
    auto const layout = computeLayout(g->type, in, kAlignasLayout, DataModel::Lp64);
    ASSERT_TRUE(layout.has_value())
        << "an un-adjusted FnSig parameter has NO layout — computeLayout nullopts";
    EXPECT_EQ(layout->size, 8u)  << "sizeof(param) is POINTER size (C: it IS a pointer)";
    EXPECT_EQ(layout->align.bytes(), 8u);
}

// FORM (2) — a DEFINITION with a NAMED parameter whose function-ness arrives
// through a FUNCTION TYPEDEF. THE SDK SHAPE: `typedef kern_return_t
// memory_reader_t(task_t, vm_address_t, vm_size_t, void **);` then
// `… (task_t, memory_reader_t reader, …)`. Distinct from form (1) because the
// declarator here carries NO `()` suffix at all — the FnSig comes from the
// head type, so a fix that keyed on the declarator's syntax would miss it.
TEST(SemanticAnalyzerC, FunctionTypedefParamAdjustsToPointerToFunction) {
    auto cu = buildShippedUnit("c", {
        "typedef int Fn(int);\n"
        "int f(Fn g, int v) { return g(v); }\n"
        "int main(void) { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u)
        << "the SDK's function-TYPEDEF parameter spelling must adjust, not reject";
    EXPECT_FALSE(model.hasErrors());
    auto const* g = findSym(model, "g");
    ASSERT_NE(g, nullptr);
    ASSERT_TRUE(adjustedFnParamPointee(model, g->type).valid())
        << "a parameter typed by a FUNCTION typedef is Ptr<FnSig>";
    // The typedef ITSELF stays a function type — the adjustment is scoped to the
    // PARAMETER, it does not rewrite the alias (else `Fn *p;` would become a
    // pointer-to-pointer and every other use of the alias would shift).
    auto const* fnAlias = findSym(model, "Fn");
    ASSERT_NE(fnAlias, nullptr);
    EXPECT_EQ(model.lattice().interner().kind(fnAlias->type), TypeKind::FnSig)
        << "the alias is still a FUNCTION type; only the PARAMETER adjusts";
}

// FORM (3) — a separate PROTOTYPE and DEFINITION of the SAME function. This is
// the FnSig-EQUALITY pin: the prototype's signature is built by the param
// HARVEST (`declRowDeclaredType`) and the definition's by the definitive
// Pass-1.5 bind. If only one of the two applied p8, the two FnSigs would be
// structurally different interned types and the merge would fail loud
// (S0022 S_IncompatibleRedeclaration) or the call would (S0003 S_TypeMismatch).
// Zero of both, with the call still dispatching, is the equality proof.
TEST(SemanticAnalyzerC, FunctionTypedefParamPrototypeAndDefinitionAgree) {
    auto model = analyzeShipped("c", {
        "typedef int Fn(int);\n"
        "int apply(Fn g, int v);\n"
        "int twice(int x) { return x + x; }\n"
        "int apply(Fn g, int v) { return g(v); }\n"
        "int main(void) { return apply(twice, 21); }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 0u)
        << "prototype and definition must build the SAME FnSig — an asymmetric "
           "p8 (applied at one resolution site only) shows up exactly here";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "and the call site must agree with both";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// FORM (4) — an ABSTRACT (type-only) parameter, `int apply(Fn, int);`. C
// 6.7.6.3p8 adjusts regardless of whether the parameter is named, so the
// abstract prototype and the NAMED definition below it must still merge. The
// abstract arm of `declRowDeclaredType` is a physically different return
// statement from the named arm — this is the test that keeps them in step.
TEST(SemanticAnalyzerC, AbstractFunctionTypedefParamAdjustsAndMergesWithNamedDefinition) {
    auto model = analyzeShipped("c", {
        "typedef int Fn(int);\n"
        "int apply(Fn, int);\n"
        "int twice(int x) { return x + x; }\n"
        "int apply(Fn g, int v) { return g(v); }\n"
        "int main(void) { return apply(twice, 21); }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 0u)
        << "`int apply(Fn, int);` and `int apply(Fn g, int v){…}` are the SAME "
           "signature — the ABSTRACT arm must adjust exactly like the named one";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u);
    EXPECT_FALSE(model.hasErrors());
    // And the harvested FnSig itself carries the ADJUSTED parameter type.
    auto const* apply = findSym(model, "apply");
    ASSERT_NE(apply, nullptr);
    TypeInterner const& in = model.lattice().interner();
    ASSERT_EQ(in.kind(apply->type), TypeKind::FnSig);
    auto const params = in.fnParams(apply->type);
    ASSERT_EQ(params.size(), 2u);
    EXPECT_TRUE(adjustedFnParamPointee(model, params[0]).valid())
        << "the FnSig's first parameter must be Ptr<FnSig>, not FnSig";
}

// FORM (5) — the parameter sits inside a NESTED fn-pointer declarator, which is
// the shape the SDK actually writes: `malloc_introspection_t`'s `enumerator`
// member is `kern_return_t (*enumerator)(task_t, …, memory_reader_t reader,
// vm_range_recorder_t recorder)`. The inner parameter list is harvested by a
// DIFFERENT walk (the fn-suffix param harvest) from a top-level definition's,
// so this is not covered by forms (1)/(2).
TEST(SemanticAnalyzerC, NestedFnPtrDeclaratorFunctionTypedefParamAdjusts) {
    auto model = analyzeShipped("c", {
        "typedef int Reader(int, int);\n"
        "typedef void Recorder(int);\n"
        "int enumerate(int task, Reader reader, Recorder recorder);\n"
        "int (*ep)(int task, Reader reader, Recorder recorder);\n"
        "int main(void) { ep = enumerate; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u)
        << "function-typed params inside a nested fn-POINTER declarator must "
           "adjust too — this is the shipped malloc_introspection_t shape";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "`ep = enumerate` type-checks ONLY if the fn-pointer's harvested "
           "params and the function's own params adjusted identically";
    EXPECT_FALSE(model.hasErrors());
    // Structural pin on the fn-POINTER's pointee signature (the harvest path).
    auto const* ep = findSym(model, "ep");
    ASSERT_NE(ep, nullptr);
    TypeInterner const& in = model.lattice().interner();
    ASSERT_EQ(in.kind(ep->type), TypeKind::Ptr);
    TypeId const sig = in.operands(ep->type)[0];
    ASSERT_EQ(in.kind(sig), TypeKind::FnSig);
    auto const params = in.fnParams(sig);
    ASSERT_EQ(params.size(), 3u);
    EXPECT_TRUE(adjustedFnParamPointee(model, params[1]).valid())
        << "the nested `Reader reader` parameter must be Ptr<FnSig>";
    EXPECT_TRUE(adjustedFnParamPointee(model, params[2]).valid())
        << "the nested `Recorder recorder` parameter must be Ptr<FnSig>";
}

// FORM (6) — a CALL THROUGH the adjusted parameter that actually DISPATCHES,
// plus the `&param` pin. `g(v)` inside the body is a call through a
// pointer-to-function; `Fn **pp = &g;` is only well-typed if the parameter is
// itself a pointer (so its address is a pointer-to-pointer-to-function — both
// facts correct per C precisely BECAUSE the parameter IS a pointer). A bare
// FnSig parameter would make `&g` a pointer-to-function and this mismatch.
TEST(SemanticAnalyzerC, CallAndAddressOfAdjustedFunctionTypeParamAreWellTyped) {
    auto model = analyzeShipped("c", {
        "typedef int Fn(int);\n"
        "int twice(int x) { return x + x; }\n"
        "int apply(Fn g, int v) {\n"
        "  Fn **pp = &g;\n"
        "  (void)pp;\n"
        "  return g(v);\n"
        "}\n"
        "int main(void) { return apply(twice, 21); }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "`&g` is pointer-to-pointer-to-function — the parameter IS a pointer";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u);
    EXPECT_FALSE(model.hasErrors())
        << "the call THROUGH the adjusted parameter must dispatch cleanly";
}

// ★ THE ANTI-VACUITY CONTROL. p8 is a PARAMETER rule and nothing else. A
// function-typed STRUCT FIELD (`struct S { Fn f; };`) is NOT a parameter, has
// no meaningful runtime form, and must STAY loud. Its row carries no
// `paramAdjustments`, so `adjustParamDeclaredType` returns it untouched and it
// still reaches the FnSig-typed-Variable reject. Without this test, a blanket
// "FnSig anywhere becomes a pointer" implementation would pass every pin above.
TEST(SemanticAnalyzerC, FunctionTypedStructFieldStillFailsLoud) {
    auto model = analyzeShipped("c", {
        "typedef int Fn(int);\n"
        "struct S { Fn f; };\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 1u)
        << "a function-typed struct MEMBER is not a parameter and must still "
           "fail loud — the adjustment is scoped to `paramAdjustments` rows";
}

// FORM (7) — the SYMBOL-CLASSIFICATION half of p8, which the type pins above
// cannot see. The INLINE spelling `int g(int)` writes the SAME syntax a function
// prototype writes — a name carrying a `()` suffix — and Pass 1's `isProto` test
// is purely syntactic, so it classified the parameter a PROTOTYPE even though
// p8 had already made its declared type a pointer. Every FORM (1) assertion
// stayed green throughout: the TYPE was right, the KIND was not. The flag then
// did real damage one tier down — a prototype re-homes onto the FILE scope, and
// CST→HIR emits no VarDecl for one — so it is pinned HERE, where it is decided.
// (`decl.paramAdjustments` suppresses `isProto`; see the derivation's comment.)
TEST(SemanticAnalyzerC, InlineFunctionTypedParamIsNotClassifiedAPrototype) {
    auto model = analyzeShipped("c", {
        "int twice(int x) { return x + x; }\n"
        "int apply(int g(int), int v) { return g(v); }\n"
        "int main(void) { return apply(twice, 21); }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* g = findSym(model, "g");
    ASSERT_NE(g, nullptr);
    EXPECT_FALSE(g->isProtoDeclaration)
        << "a PARAMETER is never a function prototype — C 6.7.6.3p8 makes its "
           "declared type a pointer, so it declares an OBJECT";
    EXPECT_EQ(g->kind, DeclarationKind::Variable)
        << "and Pass 1.5 must not upgrade it to Function (that upgrade is what "
           "makes a bare `int f(int);` callable — a parameter is not that)";
}

// FORM (8) — C 6.2.1p4: a parameter name is scoped to its own declarator and
// has NO linkage, so two functions may reuse one. The prototype
// misclassification ALSO re-homed the parameter onto the FILE scope (that is
// what D-CSUBSET-BLOCK-SCOPE-PROTOTYPE does for a real prototype), where two
// same-named parameters met as ONE symbol: MEASURED as S0022 on `g`, pointing
// at the OTHER function's parameter. Distinct symbols in distinct scopes is the
// only shape in which that cannot recur.
TEST(SemanticAnalyzerC, SameNamedInlineFunctionTypedParamsDoNotCollide) {
    auto model = analyzeShipped("c", {
        "int  twice(int x)   { return x * 2; }\n"
        "long addOne(long x) { return x + 1; }\n"
        "int a(int  g(int),  int  v) { return g(v); }\n"
        "int b(long g(long), long v) { return (int)g(v); }\n"
        "int main(void) { return a(twice, 20) + b(addOne, 1); }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 0u)
        << "two functions' parameters merely SHARE a name — a parameter has no "
           "linkage (C 6.2.1p4) and must never merge across functions";
    EXPECT_FALSE(model.hasErrors());
    // TWO symbols named `g`, in DIFFERENT scopes, with DIFFERENT pointees. A
    // re-homed pair collapses to one symbol, which fails the count first.
    std::vector<SymbolRecord const*> gs;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "g") gs.push_back(&model.symbols()[i]);
    ASSERT_EQ(gs.size(), 2u) << "each function owns its OWN `g`";
    EXPECT_NE(gs[0]->scope.v, gs[1]->scope.v)
        << "and they live in DIFFERENT scopes — neither is at file scope";
    TypeInterner const& in = model.lattice().interner();
    TypeId const p0 = adjustedFnParamPointee(model, gs[0]->type);
    TypeId const p1 = adjustedFnParamPointee(model, gs[1]->type);
    ASSERT_TRUE(p0.valid() && p1.valid());
    EXPECT_EQ(in.kind(in.fnResult(p0)), TypeKind::I32);   // `int  g(int)`
    EXPECT_EQ(in.kind(in.fnResult(p1)), TypeKind::I64);   // `long g(long)`
}

// ── D-CSUBSET-STATIC-ASSERT-OPERAND-DIAGNOSTIC ───────────────────────────────
//
// THE INSTRUMENT, not a feature: S0029 used to print the author's string and
// nothing else. MEASURED on sqlite's `src/mem1.c`, its three S0029 all come
// from ONE macro (`xnu_static_assert_struct_size`, $SDK/usr/include/mach/
// port.h) instantiated ~25× in `mach/message.h`, so all three shared one
// span, one condition source text and one author string — three literally
// indistinguishable errors. Appending the condition's SOURCE TEXT would have
// produced three byte-identical strings; only the FOLDED VALUES discriminate.
//
// AGNOSTICISM: the operator verb is read from the config-declared
// `hirLowering.binaryOps` token→verb table, never from a hardcoded `==`/`<`
// spelling or a rule name. Two different verbs are pinned below for exactly
// that reason.
//
// ⚠ The suffix APPENDS. `Int128FalseConstantExpressionStillFailsAsAssertion`
// (above) substring-matches "static assertion failed"; that prefix is
// deliberately unchanged.

// The `==` case, with real folded values on both sides.
TEST(SemanticAnalyzerC, StaticAssertFailureAppendsFoldedOperandsForEquality) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(sizeof(int) == 8, \"struct changed size unexpectedly\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    std::string text;
    std::size_t n = 0;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_StaticAssertFailed) continue;
        ++n; text = d.actual;
    }
    ASSERT_EQ(n, 1u);
    EXPECT_NE(text.find("static assertion failed"), std::string::npos)
        << "the existing prefix must be UNCHANGED (it is substring-matched "
           "elsewhere); got: " << text;
    EXPECT_NE(text.find("(folded: 4 Eq 8)"), std::string::npos)
        << "BOTH folded operands and the config-declared verb must be reported "
           "— that is the entire point of the instrument; got: " << text;
}

// The SECOND VERB. `<` proves the suffix reads `hirLowering.binaryOps` rather
// than hardcoding equality: nothing in the engine names `LtOp` or `Lt`, so a
// verb-specific implementation reds here while passing the `==` pin above.
TEST(SemanticAnalyzerC, StaticAssertFoldedOperandSuffixIsVerbAgnostic) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(4 < 3, \"four is not less than three\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    std::string text;
    for (auto const& d : model.diagnostics().all())
        if (d.code == DiagnosticCode::S_StaticAssertFailed) text = d.actual;
    EXPECT_NE(text.find("(folded: 4 Lt 3)"), std::string::npos)
        << "a NON-equality comparison must report its own config verb; got: "
        << text;
}

// ★ THE PROPERTY THE INSTRUMENT EXISTS FOR: two failing assertions that share
// the SAME author message must produce DIFFERENT diagnostic text. This is the
// `mem1.c` situation reduced to two lines — before the widening both messages
// were byte-identical and the census collapsed them.
TEST(SemanticAnalyzerC, StaticAssertFailuresSharingOneMessageAreDistinguishable) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(sizeof(int) == 8, \"struct changed size unexpectedly\");\n"
        "_Static_assert(sizeof(long) == 4, \"struct changed size unexpectedly\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    std::vector<std::string> texts;
    for (auto const& d : model.diagnostics().all())
        if (d.code == DiagnosticCode::S_StaticAssertFailed)
            texts.push_back(d.actual);
    ASSERT_EQ(texts.size(), 2u) << "both assertions must fail loud";
    EXPECT_NE(texts[0], texts[1])
        << "two failures sharing one author string must be TELLABLE APART — "
           "this is the whole instrument; got both as: " << texts[0];
    EXPECT_NE(texts[0].find("(folded: 4 Eq 8)"), std::string::npos) << texts[0];
    EXPECT_NE(texts[1].find("(folded: 8 Eq 4)"), std::string::npos) << texts[1];
}

// DEGRADE CLEANLY: a NON-binary condition still emits, still carries the author
// string, and appends nothing. No crash, no guessed operands.
TEST(SemanticAnalyzerC, StaticAssertNonBinaryConditionEmitsWithoutOperandSuffix) {
    auto cu = buildShippedUnit("c", {
        "_Static_assert(0, \"plain zero\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    std::string text;
    std::size_t n = 0;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_StaticAssertFailed) continue;
        ++n; text = d.actual;
    }
    ASSERT_EQ(n, 1u) << "a non-binary condition must STILL fail loud";
    EXPECT_NE(text.find("static assertion failed: \"plain zero\""),
              std::string::npos) << text;
    EXPECT_EQ(text.find("(folded:"), std::string::npos)
        << "there are no binary operands to report — append nothing rather "
           "than guess; got: " << text;
}

// The NON-CONSTANT branch also degrades cleanly: `sizeof` of an incomplete
// array does not fold, so the LHS renders as `<non-constant>` while the RHS
// still folds. The message keeps its own "is not an integer constant
// expression" wording (the two failure modes stay distinguishable on one code).
TEST(SemanticAnalyzerC, StaticAssertNonConstantOperandRendersAsNonConstant) {
    auto cu = buildShippedUnit("c", {
        "typedef int T[];\n"
        "_Static_assert(sizeof(T) == 4, \"incomplete has no size\");\n"
        "int main(void){ return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    std::string text;
    for (auto const& d : model.diagnostics().all())
        if (d.code == DiagnosticCode::S_StaticAssertFailed) text = d.actual;
    EXPECT_NE(text.find("is not an integer constant expression"),
              std::string::npos) << text;
    EXPECT_NE(text.find("(folded: <non-constant> Eq 4)"), std::string::npos)
        << "a side that does not fold must say so, not be omitted or guessed; "
           "got: " << text;
}

// ── D-CSUBSET-INCOMPLETE-ARRAY-TYPEDEF (C 6.7.6.2p1) ─────────────────────────
//
// "If the size is not present, the array type is an incomplete type." A typedef
// may therefore NAME one. MEASURED as sqlite's blocker:
// $SDK/usr/include/mach/vm_region.h `#define VM_PAGE_INFO_MAX` is an EMPTY
// object-like macro, so `:357` `typedef int vm_page_info_data_t[VM_PAGE_INFO_MAX];`
// expands to `typedef int vm_page_info_data_t[];` and was S000B.
//
// THE ADMISSION IS THE `typeAliasRow` SIGNAL — deliberately NOT
// `allowFlexibleArray`, which the `typedefDecl` row does not carry and MUST
// NOT be given (the interaction guard at the foot of the NEXT block states why,
// and is the pin that killed that first attempt). `typeAliasRow` is DERIVED
// from the row's already config-declared `kind: type` — literally
// `typeAliasRow = decl.kind == DeclarationKind::Type` at both declarator-mode
// minting sites in semantic_analyzer.cpp — and threaded unchanged through
// `declaratorDeclaredType` → `directDeclaredType` → `applyDeclaratorSuffix`,
// where its arm sits BELOW every present-bound return. So a present-but-non-
// constant bound (a VLA typedef) is out of the arm's reach BY CONSTRUCTION,
// not by ordering luck. The legacy `applyArraySuffix` twin carries the same
// `decl.kind == DeclarationKind::Type` disjunct under the same absent-length
// gate — it is unreachable today (no shipped row declares an `arraySuffix`
// facet) and is kept arm-for-arm in step anyway, which is why the two
// resolvers cannot drift.
//
// ★★ THE FAIL-LOUD HALF IS WHAT MAKES THE ADMISSION SAFE and it is pinned
// case by case below: an incomplete array silently sized 0 at a use site is
// precisely the silent-miscompile class the bar forbids.

// ADMIT: the typedef itself resolves, and to an INCOMPLETE array — not to a
// zero-length one and not to the element type. RED-ON-DISABLE: delete the
// `if (typeAliasRow) return …incompleteArray(inner);` arm from
// `applyDeclaratorSuffix` (semantic_analyzer.cpp) — the absent bound then falls
// straight into the `emit(S_NonConstantArrayLength)` below it and this is S000B
// again.
TEST(SemanticAnalyzerC, IncompleteArrayTypedefIsAdmittedAsIncompleteArray) {
    auto model = analyzeShipped("c", {
        "typedef int T[];\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u)
        << "`typedef int T[];` is legal C — the absent bound names an "
           "INCOMPLETE array type (C 6.7.6.2p1)";
    EXPECT_FALSE(model.hasErrors());
    auto const* t = findSym(model, "T");
    ASSERT_NE(t, nullptr);
    ASSERT_TRUE(t->type.valid());
    TypeInterner const& in = model.lattice().interner();
    EXPECT_EQ(in.kind(t->type), TypeKind::Array);
    EXPECT_TRUE(in.isIncompleteArray(t->type))
        << "it must be the INCOMPLETE array — a silently 0-sized array here is "
           "the miscompile this whole block guards against";
}

// The MACRO-EXPANDED spelling, byte-for-byte the SDK's: an EMPTY object-like
// macro in the bound position. Pinned separately from the bare `[]` because it
// is the form that actually ships and it exercises the preprocessor→parser
// hand-off, not just the semantic tier.
TEST(SemanticAnalyzerC, EmptyMacroArrayBoundTypedefMatchesTheSdkSpelling) {
    auto model = analyzeShipped("c", {
        "#define VM_PAGE_INFO_MAX\n"
        "typedef int vm_page_info_data_t[VM_PAGE_INFO_MAX];\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u)
        << "the shipped mach/vm_region.h spelling must compile";
    EXPECT_FALSE(model.hasErrors());
    auto const* t = findSym(model, "vm_page_info_data_t");
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(model.lattice().interner().isIncompleteArray(t->type));
}

// FAIL LOUD (1): a file-scope OBJECT of the incomplete type. C 6.7p7 — an
// object shall have a complete type. Silently sizing it 0 is the exact defect
// class; S0028 S_IncompleteTypeObject is the guard (widened from
// incomplete-COMPOSITE to also cover incomplete ARRAY in the same commit).
TEST(SemanticAnalyzerC, IncompleteArrayTypedefFileScopeObjectFailsLoud) {
    auto model = analyzeShipped("c", {
        "typedef int T[];\n"
        "T x;\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompleteTypeObject), 1u)
        << "an OBJECT of incomplete-array type must fail loud, never be sized 0";
}

// FAIL LOUD (2): the same at BLOCK scope — a different declaration row
// (varDecl, not topLevelDecl), so it is a genuinely separate site.
TEST(SemanticAnalyzerC, IncompleteArrayTypedefLocalObjectFailsLoud) {
    auto model = analyzeShipped("c", {
        "typedef int T[];\n"
        "int main(void) { T x; (void)x; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompleteTypeObject), 1u)
        << "a block-scope object of incomplete-array type must fail loud too";
}

// FAIL LOUD (3): `sizeof(T)`. computeLayout has no size for a bare incomplete
// array, so a const-expr use refuses — it never folds to 0.
TEST(SemanticAnalyzerC, SizeofIncompleteArrayTypedefFailsLoud) {
    auto cu = buildShippedUnit("c", {
        "typedef int T[];\n"
        "int probe[sizeof(T)];\n"
        "int main(void) { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 1u)
        << "`sizeof(T)` on an incomplete array must REFUSE — folding it to 0 "
           "would silently declare `int probe[0]`";
}

// FAIL LOUD (4): a NON-LAST member of that type. The shared composite guard
// owns this; no special case was added for typedef-sourced incompleteness.
TEST(SemanticAnalyzerC, IncompleteArrayTypedefNonLastMemberFailsLoud) {
    auto model = analyzeShipped("c", {
        "typedef int T[];\n"
        "struct S { T a; int b; };\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_FlexibleArrayNotLast), 1u)
        << "a non-last incomplete-array member would overlay the fields after "
           "it — loud, never laid out";
}

// FAIL LOUD (5): the SOLE member (C99 6.7.2.1 requires at least one other).
TEST(SemanticAnalyzerC, IncompleteArrayTypedefSoleMemberFailsLoud) {
    auto model = analyzeShipped("c", {
        "typedef int T[];\n"
        "struct S { T a; };\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_FlexibleArraySoleMember), 1u);
}

// THE ONE ADMITTED POSITION, stated explicitly so the fail-loud set above is
// read as exhaustive rather than as "members are rejected": last AND non-sole
// is a flexible array member and lays out as one — 0 bytes, element alignment,
// `sizeof` of the struct unchanged. MEASURED against /usr/bin/clang on
// arm64-darwin: sizeof 8, offsetof(a) 8, _Alignof 4.
TEST(SemanticAnalyzerC, IncompleteArrayTypedefTrailingMemberIsAFlexibleArrayMember) {
    auto cu = buildShippedUnit("c", {
        "typedef int T[];\n"
        "struct S { int a; int b; T t; };\n"
        "struct S v;\n"
        "int main(void) { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_FALSE(model.hasErrors());
    auto const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    TypeInterner const& in = model.lattice().interner();
    auto const layout = computeLayout(v->type, in, kAlignasLayout, DataModel::Lp64);
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->size, 8u)  << "clang: sizeof == 8 — the FAM adds nothing";
    EXPECT_EQ(layout->align.bytes(), 4u);
    ASSERT_EQ(layout->fieldOffsets.size(), 3u);
    EXPECT_EQ(layout->fieldOffsets[2], 8u) << "clang: offsetof(t) == 8";
}

// `extern T x;` stays legal — an extern object is completed in another TU,
// exactly as `extern char v[];` already is. The fail-loud guard must not have
// widened into this.
TEST(SemanticAnalyzerC, ExternIncompleteArrayTypedefObjectStaysLegal) {
    auto model = analyzeShipped("c", {
        "typedef int T[];\n"
        "extern T x;\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompleteTypeObject), 0u)
        << "an EXTERN incomplete-array object is legal C (completed elsewhere)";
    EXPECT_FALSE(model.hasErrors());
}

// ── D-CSUBSET-ZERO-LENGTH-ARRAY-MEMBER (GNU 6.18) ────────────────────────────
//
// MEASURED: `uint64_t ns_threadids[0];` — the last member of `struct
// netfs_status`, $SDK/usr/include/sys/mount.h, reached by sqlite — was
// S000C S_ArrayLengthOutOfRange from the `*len <= 0` reject.
//
// ★ ONE MECHANISM, per the registry row's instruction to look for the shared
// chokepoint first: a bound folding to exactly 0 on a row that admits a
// flexible array member routes into the SAME `incompleteArray` the absent-bound
// `[]` builds, in BOTH array-length resolvers. `[0]` and `[]` therefore cannot
// diverge on layout or on position — which is what the two pins below prove by
// comparing them field-for-field rather than by asserting each separately.

// LAYOUT: adding a trailing `[0]` member changes NOTHING about the struct's
// size. Exact pins, MEASURED against /usr/bin/clang -std=c17 on arm64-darwin:
// `struct P{int a;int b;}` and `struct Z{int a;int b;int t[0];}` are both
// sizeof 8 / _Alignof 4, and offsetof(Z.t) == 8.
// RED-ON-DISABLE: without the `*len == 0` arm the member is S000C and `Z` never
// composes, so every pin below reds.
TEST(SemanticAnalyzerC, TrailingZeroLengthArrayMemberLeavesStructSizeUnchanged) {
    auto cu = buildShippedUnit("c", {
        "struct P { int a; int b; };\n"
        "struct Z { int a; int b; int t[0]; };\n"
        "struct P p; struct Z z;\n"
        "int main(void) { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayLengthOutOfRange), 0u)
        << "a TRAILING `int t[0];` member is the GNU zero-length array — admit "
           "it, do not reject the bound";
    EXPECT_FALSE(model.hasErrors());
    TypeInterner const& in = model.lattice().interner();
    auto const* p = findSym(model, "p");
    auto const* z = findSym(model, "z");
    ASSERT_NE(p, nullptr); ASSERT_NE(z, nullptr);
    auto const pl = computeLayout(p->type, in, kAlignasLayout, DataModel::Lp64);
    auto const zl = computeLayout(z->type, in, kAlignasLayout, DataModel::Lp64);
    ASSERT_TRUE(pl.has_value()); ASSERT_TRUE(zl.has_value());
    EXPECT_EQ(pl->size, 8u)  << "clang: sizeof(struct P) == 8";
    EXPECT_EQ(zl->size, 8u)  << "clang: sizeof(struct Z) == 8 — the `[0]` member "
                                "contributes ZERO bytes";
    EXPECT_EQ(zl->size, pl->size)
        << "adding a trailing zero-length member must not change sizeof";
    EXPECT_EQ(zl->align.bytes(), 4u) << "clang: _Alignof(struct Z) == 4";
    ASSERT_EQ(zl->fieldOffsets.size(), 3u);
    EXPECT_EQ(zl->fieldOffsets[2], 8u) << "clang: offsetof(struct Z, t) == 8";
}

// ONE MECHANISM, pinned directly: `int t[0];` and `int t[];` must produce the
// SAME layout. A parallel implementation of zero-length arrays would drift here
// (different element handling, a real 0-length Array rather than the incomplete
// one, a struct that reports `hasFlexibleArrayMember` for only one of the two).
TEST(SemanticAnalyzerC, ZeroLengthAndAbsentBoundMembersLayOutIdentically) {
    auto cu = buildShippedUnit("c", {
        "struct Z { int a; int b; int t[0]; };\n"
        "struct F { int a; int b; int t[]; };\n"
        "struct Z z; struct F f;\n"
        "int main(void) { return 0; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_FALSE(model.hasErrors());
    TypeInterner const& in = model.lattice().interner();
    auto const* z = findSym(model, "z");
    auto const* f = findSym(model, "f");
    ASSERT_NE(z, nullptr); ASSERT_NE(f, nullptr);
    auto const zl = computeLayout(z->type, in, kAlignasLayout, DataModel::Lp64);
    auto const fl = computeLayout(f->type, in, kAlignasLayout, DataModel::Lp64);
    ASSERT_TRUE(zl.has_value()); ASSERT_TRUE(fl.has_value());
    EXPECT_EQ(zl->size, fl->size);
    EXPECT_EQ(zl->align.bytes(), fl->align.bytes());
    ASSERT_EQ(zl->fieldOffsets.size(), fl->fieldOffsets.size());
    EXPECT_EQ(zl->fieldOffsets[2], fl->fieldOffsets[2]);
    EXPECT_TRUE(zl->hasFlexibleArrayMember)
        << "`[0]` IS the GNU spelling of a flexible array member — one "
           "mechanism, not a parallel one";
    EXPECT_EQ(zl->hasFlexibleArrayMember, fl->hasFlexibleArrayMember);
}

// FAIL LOUD: a NON-TRAILING `[0]` member. It would overlay the members that
// follow it; the shared composite guard rejects it with no special case here.
TEST(SemanticAnalyzerC, NonTrailingZeroLengthArrayMemberFailsLoud) {
    auto model = analyzeShipped("c", {
        "struct S { int t[0]; int b; };\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_FlexibleArrayNotLast), 1u)
        << "only a TRAILING zero-length member is admitted";
}

// FAIL LOUD: the SOLE member.
TEST(SemanticAnalyzerC, SoleZeroLengthArrayMemberFailsLoud) {
    auto model = analyzeShipped("c", {
        "struct S { int t[0]; };\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_FlexibleArraySoleMember), 1u);
}

// FAIL LOUD: `[0]` OUTSIDE a struct member — a file-scope object, a block-scope
// object and a parameter. None of those rows admits a flexible array member, so
// none admits `[0]`: the out-of-range reject is untouched for them. This is the
// scoping pin — without it the change would read as "zero-length arrays are
// now legal everywhere".
TEST(SemanticAnalyzerC, ZeroLengthArrayOutsideAStructMemberStillFailsLoud) {
    auto model = analyzeShipped("c", {
        "int g[0];\n"
        "int take(int p[0]);\n"
        "int main(void) { int l[0]; (void)l; return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayLengthOutOfRange), 3u)
        << "a global, a local and a parameter `[0]` each stay S000C — the "
           "admission is scoped to rows that admit a flexible array member";
}

// FAIL LOUD: `int a[0] = {1};`. THE `allowInitInferredArray` EXCLUSION. The
// flag reaching the resolver is `decl.allowFlexibleArray || initNode.valid()`,
// so without the exclusion an initialized `[0]` would stop being S000C and get
// SILENTLY re-sized to `int[1]` by the initializer backfill — a written bound
// overwritten without a word. RED-ON-DISABLE: drop `&& !allowInitInferredArray`
// and this count drops to 0.
TEST(SemanticAnalyzerC, ZeroLengthArrayWithInitializerStillFailsLoud) {
    auto model = analyzeShipped("c", {
        "int a[0] = {1};\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayLengthOutOfRange), 1u)
        << "an initialized `[0]` must stay loud — never silently re-sized from "
           "its initializer";
}

// The NEGATIVE bound is untouched: `[-1]` is out of range in every position,
// including a struct member. Only EXACTLY 0 routes to the flexible-array path.
TEST(SemanticAnalyzerC, NegativeArrayBoundStillFailsLoudEvenInAStructMember) {
    auto model = analyzeShipped("c", {
        "struct S { int a; int t[-1]; };\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayLengthOutOfRange), 1u)
        << "only a bound of EXACTLY 0 is the GNU zero-length array; a negative "
           "one stays out of range";
}

// ★★ THE INTERACTION GUARD FOR D-CSUBSET-INCOMPLETE-ARRAY-TYPEDEF, and it is
// here because the first implementation FAILED IT. Admitting the absent bound
// by giving the typedefDecl row `allowFlexibleArray` looks equivalent and is
// not: that flag is tested ABOVE the VLA arm (a struct field's `int a[n]` is
// deliberately a FAM, not a VLA member), so a VLA TYPEDEF stopped building a
// vlaArray and silently became an INCOMPLETE array — a wrong type, not a
// diagnostic. The shipped signal is `typeAliasRow`, derived from the row's
// `kind: type`, and its arm sits BELOW every present-bound return, so a
// present-but-non-constant bound is structurally out of its reach — which is
// the whole reason the two are not one flag.
// (`VlaTypedefObjectAcceptsAndRecordsOrigin`
// above is the pin that caught it; this is the same property stated against
// THIS anchor so the coupling is not rediscovered by accident.)
TEST(SemanticAnalyzerC, VlaTypedefStaysAVlaAfterIncompleteArrayTypedefAdmission) {
    auto model = analyzeShipped("c", {
        "int main(void) {\n"
        "  volatile int s = 4;\n"
        "  int n = s;\n"
        "  typedef int R[n];\n"
        "  R a;\n"
        "  a[0] = 1;\n"
        "  return a[0];\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* r = findSym(model, "R");
    ASSERT_NE(r, nullptr);
    TypeInterner const& in = model.lattice().interner();
    EXPECT_TRUE(in.isVlaArray(r->type))
        << "a PRESENT-but-non-constant bound in a typedef is a VLA — the "
           "incomplete-array admission must not swallow it";
    EXPECT_FALSE(in.isIncompleteArray(r->type))
        << "and it must NOT have become the incomplete array (the silently "
           "wrong type the first implementation produced)";
}

// SCOPING, stated as a pin so the two anchors are not read as one rule:
// D-CSUBSET-ZERO-LENGTH-ARRAY-MEMBER is keyed on `allowFlexibleArray`, which
// the typedef row does not carry, so `typedef int T[0];` is NOT admitted. Only
// the ABSENT bound is a typedef-nameable incomplete type.
TEST(SemanticAnalyzerC, ZeroLengthBoundInATypedefStillFailsLoud) {
    auto model = analyzeShipped("c", {
        "typedef int T[0];\n"
        "int main(void) { return 0; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayLengthOutOfRange), 1u)
        << "the zero-length-array extension is a struct-MEMBER rule; a typedef "
           "gets the absent-bound rule only";
}

// ── TF-C97 (D-FFI-DESCRIPTOR-CROSS-FILE-TYPE-IDENTITY): ONE file-scope TAG, ──
// ── ONE TYPE — across the descriptor / source boundary (C 6.2.3)            ──
//
// The sqlite `os_unix.c` shape, and the LAST error on the arm64-macho leg:
//     struct timespec conchModTime;          /* :7711 */
//     conchModTime = buf.st_mtimespec;       /* :7731 → error[S0003] */
//
// ★★ WHAT THESE PINS ASSERT IS THE **ASSIGNMENT**, NEVER THE TAG RESOLUTION,
// and that is the whole point. The tag BINDING was never the wrong one — a
// `struct timespec x;` declaration has always resolved. What was wrong is the
// MEMBER: `sys/stat.json`'s `st_mtimespec` is interned against the DESCRIPTOR's
// `timespec` when the descriptor loads, while the tag in that TU is claimed by a
// SOURCE declaration whose tree-root binding shadows the descriptor's cuRoot one.
// A tag-resolution test is therefore VACUOUS here: it stays GREEN with the defect
// fully present. (MEASURED, this cycle: `struct timespec conchModTime;` resolved
// clean while the very next line failed.)
//
// ⚠ AND A MEASURED CORRECTION TO THE ORIGINAL DIAGNOSIS, recorded so it cannot
// be re-proposed: the second party is NOT a second DESCRIPTOR. `time.json` and
// `sys/stat.json` both declare `timespec` and they already intern ONE TypeId —
// the interner's complete-at-once path keys a composite on its FIELD CONTENT, so
// two byte-identical rows collapse, and `ShippedTypeConsistency` would have
// reported a conflict had they not (`DescriptorOnlyTimespecIsAlreadyOneType`
// below pins exactly that, and it is green on BOTH sides of this change). The
// real second party is the macOS SDK's `sys/_types/_timespec.h`, reached through
// `sys/fcntl.h` — a header DSS ships no descriptor for, so its `struct timespec`
// is parsed as SOURCE. `SourceTagUnifiesWithDescriptorMember` is the pin that
// goes RED when the fix is reverted.

namespace {

// The real-shipped-descriptor analysis used by these pins. Unlike
// `analyzeRealTgmath` it threads the ACTIVE TARGET (`arch`) — `struct stat`'s
// `st_mtimespec` lives only in the `when:{format:macho}` variant — and it passes
// `AggregateLayoutParams`, which is REQUIRED: the unification is licensed by the
// layout engine, so a caller that supplies no layout params gets no unification
// at all (the gate in semantic_analyzer.cpp), by design.
[[nodiscard]] SemanticModel analyzeRealShippedForTarget(
    std::string mainSrc, ObjectFormatKind format, std::string_view arch,
    DataModel dataModel) {
    fs::path const shipped = findRealShippedLibsDir();   // throws if unresolvable
    auto schema = loadShippedSchema("c");
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addSystemDir(shipped);
    builder.setActiveFormat(format);
    builder.addInMemory(std::move(mainSrc), "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), dataModel,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                   std::nullopt, format, arch);
}

// The symbol a SOURCE declaration minted (a valid `tree`), vs the one descriptor
// injection minted (`InvalidTree`). Both spell `timespec`, so every identity
// assertion below has to say WHICH one it means — `findSym`'s first-match would
// be an accident waiting to flip.
[[nodiscard]] SymbolRecord const* findSourceSym(SemanticModel const& m,
                                                std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name && m.symbols()[i].tree.valid())
            return &m.symbols()[i];
    return nullptr;
}
[[nodiscard]] SymbolRecord const* findInjectedSym(SemanticModel const& m,
                                                  std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name && !m.symbols()[i].tree.valid())
            return &m.symbols()[i];
    return nullptr;
}

// The xnu `sys/_types/_timespec.h` declaration verbatim (`__darwin_time_t` is
// `long` on LP64) — so it interns {i64 "long", i64 "long"} against the
// descriptors' bare {i64, i64}: the SAME 16 bytes, DIFFERENT TypeIds. That gap
// is why a TypeId-equality comparison alone can never license this unification,
// and why the layout engine is the authority.
constexpr char const* kSourceTimespecDecl =
    "struct timespec { long tv_sec; long tv_nsec; };\n";

// The os_unix.c body, reduced: poison, stat, the BY-VALUE assignment, then read
// the nested members back.
constexpr char const* kMtimespecAssignBody =
    "int main(void) {\n"
    "    struct stat sb;\n"
    "    struct timespec conchModTime;\n"
    "    conchModTime.tv_sec = -1;\n"
    "    if (stat(\"/\", &sb) != 0) return 1;\n"
    "    conchModTime = sb.st_mtimespec;\n"
    "    return (int)(conchModTime.tv_sec + conchModTime.tv_nsec);\n"
    "}\n";

} // namespace

// ★★★ THE CLOSING PIN. A SOURCE declaration of the tag and the descriptor's
// `st_mtimespec` member are ONE type, so the by-value assignment COMPILES.
// RED-ON-DISABLE (MEASURED by demonstration this cycle, against the real
// compiler on the real corpus, not just here): revert the member adoption and
// sqlite `os_unix.c` returns to exactly one `error[S0003] got buf.st_mtimespec`
// — the same diagnostic this TU produces without it.
TEST(SemanticAnalyzerC, SourceTagUnifiesWithDescriptorMember) {
    auto model = analyzeRealShippedForTarget(
        std::string{"#include <time.h>\n#include <sys/stat.h>\n"}
            + kSourceTimespecDecl + kMtimespecAssignBody,
        ObjectFormatKind::MachO, "arm64", DataModel::Lp64);

    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "`conchModTime = sb.st_mtimespec` is the assignment this anchor "
           "exists for — an S0003 here IS the defect";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::F_ShippedTypeIdentityConflict), 0u)
        << "the two declarations lay out identically, so the unification is "
           "licensed and nothing is reported";
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);

    // The IDENTITY itself, asserted directly and in BOTH directions so a green
    // result cannot come from a weakened assignment check.
    SymbolRecord const* member = findInjectedSym(model, "st_mtimespec");
    SymbolRecord const* userTag = findSourceSym(model, "timespec");
    SymbolRecord const* shippedTag = findInjectedSym(model, "timespec");
    ASSERT_NE(member, nullptr);
    ASSERT_NE(userTag, nullptr);
    ASSERT_NE(shippedTag, nullptr);
    EXPECT_EQ(member->type.v, userTag->type.v)
        << "the descriptor's member must denote the ONE type its tag names in "
           "this translation unit (C 6.2.3)";
    EXPECT_NE(member->type.v, shippedTag->type.v)
        << "and it must have MOVED off the descriptor's own pre-interned type — "
           "equal here would mean the adoption never ran";

    // Same 16 bytes either way: the unification never changes the layout.
    TypeInterner const& in = model.lattice().interner();
    AggregateLayoutParams const params{ScalarAlignmentRule::Natural, 16};
    auto const userLay = computeLayout(userTag->type, in, params, DataModel::Lp64);
    auto const shippedLay = computeLayout(shippedTag->type, in, params, DataModel::Lp64);
    ASSERT_TRUE(userLay.has_value());
    ASSERT_TRUE(shippedLay.has_value());
    EXPECT_EQ(userLay->size, 16u);
    EXPECT_EQ(shippedLay->size, 16u);
}

// THE PREMISE PIN — and it is labelled as such on purpose. Two DESCRIPTORS
// declaring `timespec` (`time.json` + `sys/stat.json`) already intern ONE
// TypeId, so the member assignment compiles with NO source declaration in sight.
// ⚠ THIS TEST IS GREEN ON BOTH SIDES OF TF-C97 — it is a guard against a future
// descriptor edit (spelling one of the two `i64 "long"`) silently splitting the
// identity, NOT evidence that the fix works. The closing pin is the one above.
TEST(SemanticAnalyzerC, DescriptorOnlyTimespecIsAlreadyOneType) {
    auto model = analyzeRealShippedForTarget(
        std::string{"#include <time.h>\n#include <sys/stat.h>\n"}
            + kMtimespecAssignBody,
        ObjectFormatKind::MachO, "arm64", DataModel::Lp64);

    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);

    SymbolRecord const* member = findInjectedSym(model, "st_mtimespec");
    SymbolRecord const* tag    = findInjectedSym(model, "timespec");
    ASSERT_NE(member, nullptr);
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(member->type.v, tag->type.v)
        << "time.json's timespec and sys/stat.json's are content-identical, so "
           "the interner's content-keyed composite path collapses them; a split "
           "here would ALSO have been reported as F_ShippedTypeIdentityConflict";
}

// FAIL LOUD, WITH THE SPECIFIC CODE. A source declaration of the same tag that
// does NOT lay out the same is REFUSED — the layout engine (the one authority)
// says 8 bytes vs 16, so the unification is reported rather than performed.
TEST(SemanticAnalyzerC, SourceTagLayoutDisagreementFailsLoud) {
    auto model = analyzeRealShippedForTarget(
        "#include <sys/stat.h>\n"
        "struct timespec { int tv_sec; int tv_nsec; };\n"
        + std::string{kMtimespecAssignBody},
        ObjectFormatKind::MachO, "arm64", DataModel::Lp64);

    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::F_ShippedTypeIdentityConflict), 1u)
        << "a 8-byte source `timespec` against the descriptor's 16-byte one must "
           "fail LOUD with the EXISTING cross-declaration identity code — never "
           "unify, and never a generic error";
    EXPECT_TRUE(model.hasErrors());
}

// ★★ THE PER-TARGET PIN — the guard against keying identity GLOBALLY, which is
// the one mistake that would have made this feature dangerous. ONE scratch
// descriptor and ONE source text, analyzed under TWO data models:
//
//   * LP64  — `long` is 64 bits, so the source `probe_ts` is 16 bytes and matches
//             the descriptor's `{i64,i64}`: unified, and the by-value assignment
//             compiles;
//   * LLP64 — `long` is 32 bits, so the SAME source text is 8 bytes: the layouts
//             disagree, the two stay DISTINCT TypeIds, and the conflict is
//             reported.
//
// Identity is keyed (tag, THIS compilation), and a compilation carries exactly
// one target/dataModel — so the LP64 and LLP64 types can never be candidates for
// each other, which is what keeps
// D-CSUBSET-DARWIN-STRUCT-LAYOUT-DISAGREEMENT closed. A global key would make
// both legs green here and silently give one of them the other's layout.
namespace {
constexpr char const* kProbeTagDescriptor =
    R"({ "header": "probe.h",
         "library": { "pe": "msvcrt.dll", "elf": "libc.so.6" },
         "structs": [
           { "name": "probe_ts",
             "fields": [ { "name": "a", "type": "i64" },
                         { "name": "b", "type": "i64" } ] },
           { "name": "probe_box",
             "fields": [ { "name": "stamp", "type": "probe_ts" },
                         { "name": "tag",   "type": "i32" } ] }
         ] })";

constexpr char const* kProbeTagSource =
    "#include <probe.h>\n"
    "struct probe_ts { long a; long b; };\n"
    "int main(void) {\n"
    "    struct probe_box bx;\n"
    "    struct probe_ts t;\n"
    "    t = bx.stamp;\n"
    "    return (int)(t.a + t.b);\n"
    "}\n";

[[nodiscard]] SemanticModel analyzeProbeTag(ScratchDir const& sysDir,
                                            DataModel dataModel) {
    auto cu = buildAngleDescriptorUnit(sysDir, "probe.json", kProbeTagDescriptor,
                                       kProbeTagSource);
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), dataModel,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}
} // namespace

TEST(SemanticAnalyzerC, CrossOriginTagUnifiesOnLp64) {
    ScratchDir sysDir{Location::Temp, "c97-probe-lp64"};
    auto model = analyzeProbeTag(sysDir, DataModel::Lp64);

    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::F_ShippedTypeIdentityConflict), 0u);
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);

    SymbolRecord const* member  = findInjectedSym(model, "stamp");
    SymbolRecord const* userTag = findSourceSym(model, "probe_ts");
    ASSERT_NE(member, nullptr);
    ASSERT_NE(userTag, nullptr);
    EXPECT_EQ(member->type.v, userTag->type.v)
        << "on LP64 `long` is 64 bits, so the two declarations lay out the same "
           "16 bytes and the tag names ONE type";
}

TEST(SemanticAnalyzerC, CrossOriginTagStaysDistinctOnLlp64) {
    ScratchDir sysDir{Location::Temp, "c97-probe-llp64"};
    auto model = analyzeProbeTag(sysDir, DataModel::Llp64);

    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::F_ShippedTypeIdentityConflict), 1u)
        << "on LLP64 `long` is 32 bits: the source struct is 8 bytes against the "
           "descriptor's 16, so the identity must be REFUSED and reported";

    SymbolRecord const* member  = findInjectedSym(model, "stamp");
    SymbolRecord const* userTag = findSourceSym(model, "probe_ts");
    ASSERT_NE(member, nullptr);
    ASSERT_NE(userTag, nullptr);
    EXPECT_NE(member->type.v, userTag->type.v)
        << "★ THE ANTI-GLOBAL-KEY ASSERTION: the SAME tag under a DIFFERENT data "
           "model must remain a DIFFERENT TypeId — a global key would have "
           "unified these and handed one leg the other's layout";
}

// ─────────────────────────────────────────────────────────────────────────────
// TF-C94 (D-CSUBSET-GNU-ATTRIBUTE): the LEADING struct/union member attribute
// position, and the `noreturn` sink that makes opening it safe.
//
// THE WITNESS: Tcl 9.0's `TclStubs` (tclDecls.h) writes
//   TCL_NORETURN1 void (*tcl_Panic) (const char *format, ...) …;
// where tcl.h expands TCL_NORETURN1 to `__attribute__ ((__noreturn__))`.
// Before this cycle the shape was `error[P0009] … got '__attribute__'`.
//
// ★★ WHY THESE TESTS ASSERT A FLAG AND NOT "no error". Opening an attribute
// POSITION whose names have no consumer converts a LOUD parse error into a
// SILENT drop — the trap `compositeAttrList`'s $packedStaysNoneComment records
// after `packed` compiled clean at the wrong `sizeof`. A member can NEVER be a
// FnSig (host clang: "field 'f' declared as a function"), so under the old
// `isFnSig`-only apply gate every `noreturn` written here would have been READ
// by `specifierPrefixNamesNoreturn` and then discarded. The gate now admits
// `Ptr<FnSig>` — which is what GNU means (the attribute binds the POINTEE's
// function type) and what host clang honors — so the fact lands on the symbol.
// ─────────────────────────────────────────────────────────────────────────────

// (a) THE TCL SHAPE. The decorated member's SymbolRecord carries isNoreturn;
// the undecorated sibling member of the SAME struct does NOT — so the flag
// tracks the SPELLING, not the position or the type.
// RED-ON-DISABLE: revert the apply gate to `isFnSig &&` → `panic` flips false
// while `plain` stays false, i.e. the attribute parses and vanishes.
TEST(SemanticAnalyzerC, NoreturnOnStructMemberFunctionPointerReachesTheSink) {
    auto cu = buildShippedUnit("c", {
        "struct TclStubs {\n"
        "    __attribute__((__noreturn__)) void (*panic)(int);\n"
        "    void (*plain)(int);\n"
        "};\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors())
        << "the leading GNU member attribute must analyze cleanly";

    SymbolRecord const* panic = findSym(model, "panic");
    SymbolRecord const* plain = findSym(model, "plain");
    ASSERT_NE(panic, nullptr);
    ASSERT_NE(plain, nullptr);
    EXPECT_TRUE(panic->isNoreturn)
        << "__attribute__((__noreturn__)) on a function-pointer MEMBER must "
           "reach SymbolRecord.isNoreturn — parsing it and dropping it is the "
           "silent-miscompile shape this position was opened to avoid";
    EXPECT_FALSE(plain->isNoreturn)
        << "an undecorated member of the same struct must stay clean — the flag "
           "must track the spelling, not the member list";
}

// (b) THE SAME SPELLING IN THE ALREADY-OPEN FILE-SCOPE POSITION, where the flag
// is not merely recorded but CONSUMED: a call through such an object lowers its
// callee to Ref(gp), which `isDirectNoreturnCall` reads (see the HIR pin
// NoreturnFunctionPointerObjectCallWrapsAndVerifies). Host clang is likewise
// SILENT here and honors it (-Wreturn-type, measured).
TEST(SemanticAnalyzerC, NoreturnOnFunctionPointerObjectReachesTheSink) {
    auto cu = buildShippedUnit("c", {
        "__attribute__((__noreturn__)) void (*gp)(int);\n"
        "void (*gp2)(int);\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors());
    SymbolRecord const* gp  = findSym(model, "gp");
    SymbolRecord const* gp2 = findSym(model, "gp2");
    ASSERT_NE(gp, nullptr);
    ASSERT_NE(gp2, nullptr);
    EXPECT_TRUE(gp->isNoreturn);
    EXPECT_FALSE(gp2->isNoreturn);
}

// (c) THE WIDENING MUST NOT LEAK. A plain data object still gets NO flag: the
// gate admits FnSig and Ptr<FnSig> ONLY, so the pre-existing
// `_Noreturn`-on-a-non-function safe-miss deferral is untouched and no data
// symbol carries a codegen directive nothing can honor. Host clang WARNS on
// this shape ("'__noreturn__' only applies to function types", measured); DSS's
// silence here is the separate open row
// D-CSUBSET-APPLIESTO-CANNOT-EXPRESS-FUNCTION-POINTER-OBJECT / the `none`-verb
// row split, NOT something this cycle introduced.
// RED-ON-DISABLE: widen the gate to "any declarator" → both EXPECTs flip.
TEST(SemanticAnalyzerC, NoreturnOnPlainDataObjectStaysInert) {
    auto cu = buildShippedUnit("c", {
        "__attribute__((__noreturn__)) int gv = 1;\n"
        "struct S { __attribute__((__noreturn__)) int m; };\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    SymbolRecord const* gv = findSym(model, "gv");
    SymbolRecord const* m  = findSym(model, "m");
    ASSERT_NE(gv, nullptr);
    ASSERT_NE(m, nullptr);
    EXPECT_FALSE(gv->isNoreturn)
        << "a non-function, non-function-pointer object must carry NO noreturn";
    EXPECT_FALSE(m->isNoreturn)
        << "the member arm must obey the same gate as the file-scope arm";
}

// (d) THE POSITION REACHES `scanAttributeSemantics`, NOT JUST THE NORETURN
// SCAN. A leading `aligned(N)` on a member is HONORED into the member symbol's
// explicitAlignment — the same sink the shipped TRAILING member slot feeds — so
// the whole effects table is live in the new position, not only one name.
TEST(SemanticAnalyzerC, StructMemberLeadingAlignedAttributeIsHonored) {
    auto cu = buildShippedUnit("c", {
        "struct S { __attribute__((aligned(16))) int a; };\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_FALSE(model.hasErrors());
    SymbolRecord const* a = findSym(model, "a");
    ASSERT_NE(a, nullptr);
    ASSERT_TRUE(a->explicitAlignment.has_value())
        << "a LEADING member `aligned(N)` must reach the alignment sink";
    EXPECT_EQ(*a->explicitAlignment, 16u);
}

// (e) AN UNRECOGNIZED GNU NAME IN THE NEW POSITION IS LOUD. structField /
// unionField carry `unknownStrictAttributeIsError: true`, and the leading slot
// inherits it because it folds into the SAME declaration's facts. Without this
// the new position would be a hole in the name axis exactly as
// D-TEST-IGNORE-LIST-IS-A-LICENSE-TO-DROP describes.
TEST(SemanticAnalyzerC, StructMemberLeadingUnknownAttributeIsLoud) {
    auto cu = buildShippedUnit("c", {
        "struct S { __attribute__((frobnicate)) int x; };\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_TRUE(model.hasErrors())
        << "an unknown GNU attribute name in the leading member position must "
           "fail loud, never be skipped";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 1u);
}

// (f) THE DECL-KIND GATE (TF-C93) IS LIVE IN THE NEW POSITION. `noinline` is
// `appliesTo: ["function"]`, a member is a variable, so the shared warning
// fires — identical to what the shipped TRAILING member slot already does
// (measured at the pre-change HEAD). One attribute must not mean two different
// things depending on which side of the declarator it is written.
TEST(SemanticAnalyzerC, StructMemberLeadingNoinlineWarnsLikeTheTrailingSlot) {
    for (char const* src : {
             "struct S { __attribute__((noinline)) int x; };\n",
             "struct S { int x __attribute__((noinline)); };\n"}) {
        auto cu = buildShippedUnit("c", { std::string(src) });
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_AttributeIgnoredForDeclarationKind),
                  1u)
            << "leading and trailing member slots must be judged identically: "
            << src;
    }
}

// (g) REGRESSION: the rule that grew the alt still carries its original
// content, and a MIXED run (alignas + attribute, either order) folds both.
TEST(SemanticAnalyzerC, StructMemberLeadingAlignasAndAttributeCompose) {
    auto cu = buildShippedUnit("c", {
        "struct S { alignas(16) __attribute__((__noreturn__)) void (*p)(int); };\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignasLayout);
    EXPECT_FALSE(model.hasErrors());
    SymbolRecord const* p = findSym(model, "p");
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(p->explicitAlignment.has_value())
        << "the alignas branch of the run must still reach its sink";
    EXPECT_EQ(*p->explicitAlignment, 16u);
    EXPECT_TRUE(p->isNoreturn)
        << "and the attribute branch beside it must still reach its own";
}

// ── TF-C112 (D-FFI-PE-CRT-UCRT-MIGRATION): a goal-2 SUPPRESSED descriptor row
//    must carry its REALIZATION, not just its library ────────────────────────
//
// A descriptor row declares two independent things: WHERE the symbol resolves
// (`library`) and WHETHER it is an import at all (`synthesize`). Goal-2 hands a
// user re-declaration authority over the SIGNATURE — never over the platform's
// realization — so a suppressed row has to forward BOTH or the second property
// is silently lost for the declarations users write most often.
//
// The cost of losing it is not theoretical and not graceful. `ucrtbase.dll`
// exports none of printf/fprintf/sprintf/vfprintf/sscanf (only the
// `__stdio_common_v*` cores), so those five pe rows are compiler-SYNTHESIZED.
// A suppressed row forwarding only the library made the user's prototype
// re-export the name as a raw eager import, and DSS eager-imports every
// declared shipped extern — so `#include <stdio.h>` + `int printf(const char*,
// ...);` compiled rc=0, emitted no diagnostic anywhere, and died at PROCESS
// START with 0xC0000139 (MEASURED at the TF-C111 HEAD).
//
// These pins are the SEMANTIC half of the fix: the row's recipe id and its
// declared signature survive suppression. The HIR half (recipe → shim, never an
// import row; and the signature refusal) is pinned in test_hir_lowering_c.

namespace {

// Build + analyze `mainSrc` against the REAL src/dss-config/shippedLibs under a
// GIVEN object format — the `analyzeRealTgmath` shape, re-declared here so these
// pins read against the descriptors the production driver ships (a regression in
// stdio.json itself flips them red rather than being mirrored green by a scratch
// copy). The format is threaded to BOTH the UnitBuilder (macro `variants` splice)
// and `analyze` (the per-symbol availability gate) exactly as the driver does —
// which is load-bearing here: stdio.json carries TWO `printf` rows, and only the
// pe one has a `synthesize` tag.
[[nodiscard]] SemanticModel analyzeRealStdio(std::string mainSrc,
                                             ObjectFormatKind format,
                                             DataModel dataModel,
                                             std::string_view arch) {
    fs::path const shipped = findRealShippedLibsDir();   // throws if unresolvable
    auto schema = loadShippedSchema("c");
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addSystemDir(shipped);
    builder.setActiveFormat(format);
    builder.addInMemory(std::move(mainSrc), "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), dataModel, std::nullopt, std::nullopt, format, arch);
}

// The reproducer, verbatim: legal C that clang and GCC both accept, and the
// single most-redeclared identifier in the language.
constexpr char const* kPrintfRedeclSrc =
    "#include <stdio.h>\n"
    "int printf(const char *fmt, ...);\n"
    "int main(void) { return printf(\"hi\n\"); }\n";

} // namespace

// PE: the suppressed row keeps its `synthesize` recipe. RED before TF-C112 —
// `SuppressedShippedSymbol` had no such field, so the answer was structurally
// unavailable.
TEST(SemanticAnalyzerC, TFC112SuppressedPeStdioRowCarriesItsSynthesizeRecipe) {
    auto model = analyzeRealStdio(kPrintfRedeclSrc, ObjectFormatKind::Pe,
                                  DataModel::Llp64, "x86_64");
    EXPECT_FALSE(model.hasErrors());
    auto const* sup = model.suppressedShippedSymbolFor("printf");
    ASSERT_NE(sup, nullptr)
        << "goal-2 suppressed the descriptor's printf — the row must be recorded";
    EXPECT_EQ(sup->recipeId, "printf")
        << "the pe row is realized as a compiler-synthesized shim; a suppressed "
           "copy that forgets the recipe re-exports it as a ucrtbase import that "
           "cannot load (0xC0000139)";
    // The per-format library still rides too — the c86/c156 contract is intact,
    // not replaced. Which ROW was recorded is decided by the availability gate,
    // and `ucrtbase.dll` here proves the PE row won (the elf/macho `printf` row
    // carries no recipe, so recording it would have looked identical to the
    // pre-fix bug).
    ASSERT_TRUE(sup->library.contains("pe"));
    EXPECT_EQ(sup->library.at("pe"), "ucrtbase.dll");
}

// PE: and the row's DECLARED SIGNATURE, which is the shim-compatibility oracle.
// Two-sided on purpose — "valid" alone would pass on any type at all. The pin is
// that it is EXACTLY the type the user's own prototype resolved to, because that
// identity is what licenses the lowerer to hand this prototype the shim.
TEST(SemanticAnalyzerC, TFC112SuppressedRowSignatureMatchesTheUserPrototype) {
    auto model = analyzeRealStdio(kPrintfRedeclSrc, ObjectFormatKind::Pe,
                                  DataModel::Llp64, "x86_64");
    EXPECT_FALSE(model.hasErrors());
    auto const* sup = model.suppressedShippedSymbolFor("printf");
    ASSERT_NE(sup, nullptr);
    ASSERT_TRUE(sup->signature.valid())
        << "without the declared signature there is no oracle to judge a "
           "re-declaration against, and the shim would be handed out blind";
    auto const* user = findSymbolNamed(model, "printf");
    ASSERT_NE(user, nullptr);
    ASSERT_TRUE(user->type.valid());
    EXPECT_EQ(user->type.v, sup->signature.v)
        << "`int printf(const char*, ...)` IS stdio.json's `fn(ptr<char>, ...) "
           "-> i32` — interner TypeId equality is structural FnSig equality, and "
           "`const` is not interned. If this ever diverges the lowerer starts "
           "REFUSING the commonest legal declaration in C, so it is pinned here "
           "rather than left to the refusal path to discover.";
    // Exactly ONE printf symbol exists: goal-2 deleted the descriptor's own
    // injection, so the user's prototype is the sole declaration and the only
    // thing that can carry the call.
    EXPECT_EQ(countSymbolsNamed(model, "printf"), 1u);
}

// ELF: the SAME source, and the row that wins there carries NO recipe — glibc
// exports a real `printf`, so it stays an ordinary FFI import. This is the
// agnosticism pin: the recipe comes from per-format DESCRIPTOR DATA selected by
// the availability gate, never from a format test in the compiler.
TEST(SemanticAnalyzerC, TFC112SuppressedElfStdioRowCarriesNoRecipe) {
    auto model = analyzeRealStdio(kPrintfRedeclSrc, ObjectFormatKind::Elf,
                                  DataModel::Lp64, "x86_64");
    EXPECT_FALSE(model.hasErrors());
    auto const* sup = model.suppressedShippedSymbolFor("printf");
    ASSERT_NE(sup, nullptr);
    EXPECT_TRUE(sup->recipeId.empty())
        << "the elf printf row is a plain libc.so.6 import — tagging it would "
           "drop a working import in favour of a shim nothing asked for";
    ASSERT_TRUE(sup->library.contains("elf"));
    EXPECT_EQ(sup->library.at("elf"), "libc.so.6");
}

// ── TF-C121: the suppressed row also carries its per-target LINK BASE NAME ────
//
// `SuppressedShippedSymbol::linkName` is the THIRD property a suppressed
// descriptor row must forward, joining `version` (c156) and `recipeId`
// (TF-C112). It shipped with NO test coverage anywhere under tests/analysis/,
// on a field whose sibling properties each got a pin ONLY AFTER the same two
// suppression sites had already dropped one of them — which is what the two
// TFC112 tests above are. Same field family, same failure mode, so the same
// guard.
//
// THE SCENARIO IS THE ONE THE FIELD'S OWN COMMENT NAMES: a user restating
// `fstat`'s C signature over `#include <sys/stat.h>`. That is legal C and
// exactly what portable code writes. The prototype restates the SIGNATURE and
// says NOTHING about which name libSystem exports for it on this arch, so a
// suppressed row that forgets the link name silently re-binds Darwin-x86_64's
// LEGACY 32-bit-inode `_fstat` — which reads st_size from offset 96 of a
// structure that writes it at 72, reporting 0. It LINKS CLEAN either way, so
// nothing fails loud and only a pin like this one can see it.
//
// BOTH ARMS, for the reason the reader-side sibling
// (`ShippedLibDescriptor.SymbolLinkNameVariantSelectsPerArchBothArms`) gives:
// x86_64 must carry the alias and arm64 must carry NOTHING, because arm64 has
// one inode ABI and its plain name is already correct. A flat string would
// satisfy one arm while corrupting the other.
//
// ⚠ SCOPE, so a green run is not over-read: this pins the SEMANTIC rail — that
// the suppressed row RECORDS the resolved name. The two CST→HIR threading sites
// that carry it onward into `HirExternRecord` (the bare-prototype arm and
// `claimSuppressedShimSymbol`) are a separate rail and are NOT pinned here; the
// shipped `shipped_linkname_inode64` corpus example exercises the ordinary
// INJECTION path, not this suppression path, so that rail's pin belongs in
// tests/hir/test_hir_lowering_c.cpp and does not exist yet.
namespace {
// `fn(i32, ptr<void>) -> i32` IS sys/stat.json's declared shape for `fstat`, so
// this prototype interns to the same TypeId and goal-2 suppression fires.
constexpr char const* kFstatRedeclSrc =
    "#include <sys/stat.h>\n"
    "int fstat(int fd, void *buf);\n"
    "int main(void) { return fstat(0, (void *)0); }\n";
} // namespace

TEST(SemanticAnalyzerC, TFC121SuppressedDarwinRowCarriesItsPerTargetLinkName) {
    auto const linkNameOn = [](std::string_view arch) -> std::string {
        auto model = analyzeRealShippedForTarget(kFstatRedeclSrc,
                                                 ObjectFormatKind::MachO, arch,
                                                 DataModel::Lp64);
        EXPECT_FALSE(model.hasErrors())
            << (model.diagnostics().all().empty()
                    ? "" : model.diagnostics().all()[0].actual);
        auto const* sup = model.suppressedShippedSymbolFor("fstat");
        if (sup == nullptr) {
            ADD_FAILURE()
                << "goal-2 must have suppressed the descriptor's fstat for arch "
                << arch << " — with no recorded row there is nothing to forward";
            return "<missing>";
        }
        return sup->linkName;
    };
    EXPECT_EQ(linkNameOn("x86_64"), "fstat$INODE64")
        << "the suppressed row must carry Darwin-x86_64's $INODE64 alias. "
           "Dropping it hands the user's prototype the LEGACY 32-bit-inode "
           "callee — a SILENT misbind: it links, it runs, and st_size reads 0";
    EXPECT_EQ(linkNameOn("arm64"), "")
        << "arm64-Darwin has ONE inode ABI, so no variant matches and the "
           "resolved name is EMPTY, meaning 'use `name`'. A flat (non-variant) "
           "link name would put `_fstat$INODE64` here and break the arch that "
           "was already correct";
}

// ── THE GNU `__`-WRAPPED QUALIFIER / SPECIFIER SPELLINGS ─────────────────────
// (D-CSUBSET-GNU-DUNDER-QUALIFIER-SPELLINGS, 2026-08-12)
//
// Ten keyword-table rows, five kinds, zero C++: `__volatile__`/`__volatile` →
// VolatileKeyword, `__const__`/`__const` → ConstKeyword, `__signed__`/`__signed`
// → SignedKeyword, `__restrict__`/`__restrict` → RestrictKeyword,
// `__complex__`/`__complex` → ComplexKeyword. Same many-words-one-kind facility
// as `inline`/`__inline`/`__inline__` (InlineGnuSpellingsAreSynonyms) and
// `__typeof__`/`__alignof__`.
//
// ★★ WHY THESE PINS ASSERT SEMANTIC EFFECTS AND NEVER "IT COMPILED".
// A parse-only test cannot tell a CORRECT alias from a PLAUSIBLE WRONG one, and
// for this particular set the wrong mappings are not far-fetched — all five
// kinds are cv/specifier tokens that share grammar slots, so `__const__` landed
// on VolatileKeyword, or `__restrict__` landed on ConstKeyword, would still
// PARSE every declaration below. What separates them is the observable each kind
// uniquely owns, and each pin below asserts that observable, so a mis-keyed row
// is red here rather than green-and-silently-wrong:
//   * VolatileKeyword — the interned Volatile QUALIFIER BIT
//     (`ti.isVolatileQualified`). No other kind sets it.
//   * ConstKeyword    — reaches `constMarker`, marks the symbol `isConst`, and
//     an assignment through it is S_ConstViolation. No other kind is loud there.
//   * SignedKeyword   — participates in the `typeSpecifiers` MULTISET, so
//     `__signed__ char` is I8 (not U8, which is where an Unsigned mis-key lands,
//     and not I32, which is where a dropped specifier lands).
//   * ComplexKeyword  — also a multiset participant: `__complex__ double` is
//     TypeKind::Complex over an F64 element, and `__complex__ int` is the LOUD
//     invalid multiset S_InvalidTypeSpecifierCombination — whereas the adjacent
//     ImaginaryKeyword sits in NO row and would give S_UnknownType instead.
//   * RestrictKeyword — has NO interned bit at all (only VolatileQual is
//     interned), so it is the one kind with no positive type observable. It is
//     pinned by WHERE IT IS REFUSED instead: `headQualifier` is
//     {Const, Volatile, Atomic} and `typeSpecifierSeq` admits Signed/Complex, so
//     EVERY other candidate kind is ACCEPTED bare in front of a base type while
//     RestrictKeyword is not. `__restrict__ int x;` must therefore be REJECTED —
//     identically to ISO `restrict int x;` — which is exactly the assertion a
//     Const/Volatile/Atomic/Signed/Complex mis-key fails.
//
// ★ MULTI-FORM, not a sampled subset: every one of the ten spellings appears,
// and each family is exercised in more than one grammar position (declaration
// head, pointer qualifier, parameter, array-suffix modifier, east/trailing
// specifier), because a row can be reachable from one slot and not another.
//
// MEASURED on this host with matched positive controls (a plain-ISO file) and a
// NEGATIVE control (`__no_such_keyword__`, rejected — so the lexer is NOT
// blanket-accepting dunder words and these ten prove something): gcc 13.3.0,
// clang 18.1.3 and clang 19.1.1 accept all ten, and `gcc -pedantic-errors`
// accepts them too — the spellings are in C 7.1.3's implementation-reserved
// namespace, so no conforming program can collide with them and no dialect flag
// is needed. ⚠ NOT claimed: real `cl.exe` does NOT accept the dunder forms of
// volatile/const/signed/restrict. Two of three reference compilers is the bar.

namespace {
// Error-severity TREE-BUILDER (parse) diagnostics across a CU. The refusal pins
// below need the POSITIVE form of what `assertNoBuilderErrors` asserts the
// absence of, and a parse refusal never reaches the semantic model — so a
// SemanticModel-based check would read clean on exactly the inputs these pins
// are about. Mirrors the fixture's own traversal (per-tree, Error severity only).
[[nodiscard]] std::size_t countBuilderErrors(CompilationUnit const& cu) {
    std::size_t n = 0;
    for (auto const& t : cu.trees())
        for (auto const& d : t.diagnostics().all())
            if (d.severity == DiagnosticSeverity::Error) ++n;
    return n;
}
} // namespace

// (1) `__volatile__` / `__volatile` set the interned VOLATILE QUALIFIER BIT —
// the observable no other candidate kind can produce. Two grammar positions:
// the declaration head (a volatile OBJECT) and after a `*` (a volatile POINTER).
TEST(SemanticAnalyzerC, GnuVolatileSpellingsQualifyLikeIsoSpelling) {
    // (a) HEAD position. `volatile int` / `__volatile__ int` / `__volatile int`
    // must be the SAME interned TypeId — not merely all volatile, but literally
    // one type, which is what "alias" means at the lattice.
    auto cu = buildShippedUnit("c", {
        "volatile int iso;\n"
        "__volatile__ int gnuLong;\n"
        "__volatile int gnuShort;\n"
        "int plain;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const& ti = model.lattice().interner();
    SymbolRecord const* iso = findSym(model, "iso");
    ASSERT_NE(iso, nullptr);
    ASSERT_TRUE(iso->type.valid());
    ASSERT_TRUE(ti.isVolatileQualified(iso->type))
        << "instrument check: the ISO spelling must set the bit, else the "
           "comparisons below are vacuous";
    for (char const* name : {"gnuLong", "gnuShort"}) {
        SymbolRecord const* s = findSym(model, name);
        ASSERT_NE(s, nullptr) << name;
        ASSERT_TRUE(s->type.valid()) << name;
        EXPECT_TRUE(ti.isVolatileQualified(s->type))
            << name << ": a GNU volatile spelling must set the SAME interned "
                       "Volatile qualifier bit `volatile` sets — this is the "
                       "assertion a row mapped to ConstKeyword (which parses "
                       "here just as happily) fails";
        EXPECT_EQ(s->type.v, iso->type.v)
            << name << ": the alias must intern to the IDENTICAL TypeId as the "
                       "ISO spelling, not merely to some volatile type";
    }
    // NEGATIVE control in the same CU: an unqualified `int` must NOT carry the
    // bit, so the pin above is discriminating rather than always-true.
    SymbolRecord const* plain = findSym(model, "plain");
    ASSERT_NE(plain, nullptr);
    ASSERT_TRUE(plain->type.valid());
    EXPECT_FALSE(ti.isVolatileQualified(plain->type))
        << "a plain `int` must not be volatile-qualified";
    EXPECT_NE(plain->type.v, iso->type.v)
        << "`volatile int` and `int` are DISTINCT interned types";
}

// (1b) POINTER-qualifier position (`int * __volatile__ p`) — a different
// grammar slot (`ptrQualifier`, not `headQualifier`), so it needs its own pin:
// a row can be reachable from one slot and not the other. Asserted against the
// ISO spelling's own answer in the same CU rather than against a hard-coded
// expectation, so the pin states "the alias agrees with `volatile`" — which is
// the claim — whether or not a post-star volatile qualifies the pointer.
TEST(SemanticAnalyzerC, GnuVolatileSpellingsInPointerQualifierPosition) {
    auto cu = buildShippedUnit("c", {
        "int * volatile pIso;\n"
        "int * __volatile__ pGnuLong;\n"
        "int * __volatile pGnuShort;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const& ti = model.lattice().interner();
    SymbolRecord const* iso = findSym(model, "pIso");
    ASSERT_NE(iso, nullptr);
    ASSERT_TRUE(iso->type.valid());
    ASSERT_EQ(ti.kind(iso->type), TypeKind::Ptr);
    for (char const* name : {"pGnuLong", "pGnuShort"}) {
        SymbolRecord const* s = findSym(model, name);
        ASSERT_NE(s, nullptr) << name;
        ASSERT_TRUE(s->type.valid()) << name;
        EXPECT_EQ(s->type.v, iso->type.v)
            << name << ": a post-star GNU volatile must resolve to the SAME "
                       "interned type the ISO spelling resolves to in the same "
                       "position";
        EXPECT_EQ(ti.isVolatileQualified(s->type),
                  ti.isVolatileQualified(iso->type))
            << name << ": the alias must agree with `volatile` about the "
                       "pointer's own qualification, whatever that answer is";
    }
}

// (2) `__const__` / `__const` reach `constMarker` and make the object CONST —
// pinned by `isConst` on the symbol AND by the S_ConstViolation an assignment
// through it must raise. A row mis-keyed to VolatileKeyword parses every line
// here and raises NOTHING, which is precisely the silent const-loss this pin
// exists to catch. Three positions: declaration head, EAST/trailing specifier,
// and a parameter.
TEST(SemanticAnalyzerC, GnuConstSpellingsMarkObjectsConst) {
    auto cu = buildShippedUnit("c", {
        "const int iso = 1;\n"
        "__const__ int gnuLong = 2;\n"
        "__const int gnuShort = 3;\n"
        "int __const__ gnuEast = 4;\n"     // EAST/trailing specifier position
        "int mut = 5;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    SymbolRecord const* iso = findSym(model, "iso");
    ASSERT_NE(iso, nullptr);
    ASSERT_TRUE(iso->isConst)
        << "instrument check: the ISO spelling must mark const, else the "
           "comparisons below are vacuous";
    for (char const* name : {"gnuLong", "gnuShort", "gnuEast"}) {
        SymbolRecord const* s = findSym(model, name);
        ASSERT_NE(s, nullptr) << name;
        EXPECT_TRUE(s->isConst)
            << name << ": a GNU const spelling must reach `constMarker` and "
                       "mark the symbol const — a row mapped to "
                       "VolatileKeyword parses this line and marks NOTHING";
    }
    // NEGATIVE control: a plain object must NOT come out const.
    SymbolRecord const* mut = findSym(model, "mut");
    ASSERT_NE(mut, nullptr);
    EXPECT_FALSE(mut->isConst) << "a plain `int` is not const";
}

// (2b) The ENFORCEMENT half: assigning through each GNU const spelling is LOUD,
// exactly as through `const`. `isConst` on the record is a flag; this is the
// behaviour that flag is for, and it is a separate tier (SE4's const check).
TEST(SemanticAnalyzerC, GnuConstSpellingsAreEnforcedOnAssignment) {
    // Instrument check: the ISO spelling raises exactly one violation.
    auto isoModel = analyzeShipped(
        "c", {"int main(void){ const int c = 5; c = 6; return c; }\n"});
    ASSERT_EQ(countCode(isoModel.diagnostics(),
                        DiagnosticCode::S_ConstViolation), 1u)
        << "instrument check: ISO `const` must be enforced here";
    for (std::string_view spelling : {"__const__", "__const"}) {
        auto model = analyzeShipped(
            "c", {"int main(void){ " + std::string(spelling) +
                         " int c = 5; c = 6; return c; }\n"});
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_ConstViolation), 1u)
            << spelling << ": assigning through a GNU const spelling must be "
                           "S_ConstViolation, exactly as through `const`";
    }
    // NEGATIVE control: without any const spelling the SAME assignment is
    // clean, so the pin above is measuring the qualifier and not the statement.
    auto mutModel = analyzeShipped(
        "c", {"int main(void){ int c = 5; c = 6; return c; }\n"});
    EXPECT_EQ(countCode(mutModel.diagnostics(),
                        DiagnosticCode::S_ConstViolation), 0u)
        << "assigning to a NON-const object must be clean";
}

// (2c) `__const__` as a POINTER qualifier (`int * __const__ p`) — the
// `ptrQualifier` slot, a different reach than the head. Pinned against the ISO
// spelling's own interned answer in the same CU.
TEST(SemanticAnalyzerC, GnuConstSpellingsInPointerQualifierPosition) {
    auto cu = buildShippedUnit("c", {
        "int * const pIso = 0;\n"
        "int * __const__ pGnuLong = 0;\n"
        "int * __const pGnuShort = 0;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const& ti = model.lattice().interner();
    SymbolRecord const* iso = findSym(model, "pIso");
    ASSERT_NE(iso, nullptr);
    ASSERT_TRUE(iso->type.valid());
    ASSERT_EQ(ti.kind(iso->type), TypeKind::Ptr);
    for (char const* name : {"pGnuLong", "pGnuShort"}) {
        SymbolRecord const* s = findSym(model, name);
        ASSERT_NE(s, nullptr) << name;
        ASSERT_TRUE(s->type.valid()) << name;
        EXPECT_EQ(s->type.v, iso->type.v)
            << name << ": a post-star GNU const must intern to the SAME type "
                       "`int * const` interns to";
    }
}

// (3) `__signed__` / `__signed` participate in the type-specifier MULTISET, so
// the resolved CORE TYPE is the pin: `__signed__ char` is I8. A row mis-keyed to
// UnsignedKeyword yields U8 and a dropped specifier yields I32 — both parse, and
// both are caught here. Four multiset shapes per spelling, including the
// bare-specifier form (`__signed__ x;` == `int`) and a two-token `long` combo.
TEST(SemanticAnalyzerC, GnuSignedSpellingsResolveLikeIsoSpelling) {
    auto cu = buildShippedUnit("c", {
        "signed char isoC;\n"
        "__signed__ char gnuLongC;\n"
        "__signed char gnuShortC;\n"
        "__signed__ gnuLongBare;\n"        // bare specifier == `signed int`
        "__signed gnuShortBare;\n"
        "__signed__ long gnuLongLong;\n"   // multi-token multiset
        "unsigned char uC;\n",             // the mis-key's destination
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const& ti = model.lattice().interner();
    SymbolRecord const* iso = findSym(model, "isoC");
    ASSERT_NE(iso, nullptr);
    ASSERT_TRUE(iso->type.valid());
    ASSERT_EQ(ti.kind(iso->type), TypeKind::I8)
        << "instrument check: ISO `signed char` must be I8";
    for (char const* name : {"gnuLongC", "gnuShortC"}) {
        SymbolRecord const* s = findSym(model, name);
        ASSERT_NE(s, nullptr) << name;
        ASSERT_TRUE(s->type.valid()) << name;
        EXPECT_EQ(ti.kind(s->type), TypeKind::I8)
            << name << ": `<gnu signed> char` is I8 — U8 would mean the row "
                       "landed on UnsignedKeyword, I32 would mean the "
                       "specifier was dropped";
        EXPECT_EQ(s->type.v, iso->type.v)
            << name << ": and it must be the IDENTICAL interned type";
    }
    for (char const* name : {"gnuLongBare", "gnuShortBare"}) {
        SymbolRecord const* s = findSym(model, name);
        ASSERT_NE(s, nullptr) << name;
        ASSERT_TRUE(s->type.valid()) << name;
        EXPECT_EQ(ti.kind(s->type), TypeKind::I32)
            << name << ": a BARE GNU signed specifier is `signed int` (I32) — "
                       "the multiset row with SignedKeyword alone";
    }
    SymbolRecord const* sl = findSym(model, "gnuLongLong");
    ASSERT_NE(sl, nullptr);
    ASSERT_TRUE(sl->type.valid());
    EXPECT_EQ(ti.kind(sl->type), TypeKind::I64)
        << "`__signed__ long` must reach the [Signed, Long] multiset row "
           "(I64 on this LP64 test target)";
    // NEGATIVE control: the type the UnsignedKeyword mis-key would produce is
    // genuinely DIFFERENT, so the I8 assertions above can actually fail.
    SymbolRecord const* u = findSym(model, "uC");
    ASSERT_NE(u, nullptr);
    ASSERT_TRUE(u->type.valid());
    EXPECT_EQ(ti.kind(u->type), TypeKind::U8);
    EXPECT_NE(u->type.v, iso->type.v)
        << "`signed char` and `unsigned char` must be distinct interned types, "
           "else the mis-key this pin guards against would be undetectable";
}

// (4) `__complex__` / `__complex` are multiset participants too: the resolved
// type is TypeKind::Complex over the right ELEMENT. Both spellings, both
// element widths, and the EAST/trailing position (`double __complex__`), since
// C 6.7.1 lets decl-specifiers appear in any order and the multiset is
// order-independent — a mis-key to ImaginaryKeyword (which sits in NO multiset
// row) makes every line here S_UnknownType instead.
TEST(SemanticAnalyzerC, GnuComplexSpellingsResolveLikeIsoSpelling) {
    auto cu = buildShippedUnit("c", {
        "double _Complex isoD;\n"
        "__complex__ double gnuLongD;\n"
        "__complex double gnuShortD;\n"
        "double __complex__ gnuEastD;\n"   // EAST/trailing specifier position
        "__complex__ float gnuLongF;\n"
        "__complex float gnuShortF;\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const& ti = model.lattice().interner();
    SymbolRecord const* iso = findSym(model, "isoD");
    ASSERT_NE(iso, nullptr);
    ASSERT_TRUE(iso->type.valid());
    ASSERT_EQ(ti.kind(iso->type), TypeKind::Complex)
        << "instrument check: ISO `double _Complex` must be Complex";
    for (char const* name : {"gnuLongD", "gnuShortD", "gnuEastD"}) {
        SymbolRecord const* s = findSym(model, name);
        ASSERT_NE(s, nullptr) << name;
        ASSERT_TRUE(s->type.valid()) << name;
        ASSERT_EQ(ti.kind(s->type), TypeKind::Complex) << name;
        EXPECT_EQ(ti.kind(ti.complexElement(s->type)), TypeKind::F64)
            << name << ": a GNU complex spelling over `double` must have an "
                       "F64 element — the ELEMENT is what a wrong multiset row "
                       "would get wrong";
        EXPECT_EQ(s->type.v, iso->type.v)
            << name << ": and it must be the IDENTICAL interned type as "
                       "`double _Complex`";
    }
    for (char const* name : {"gnuLongF", "gnuShortF"}) {
        SymbolRecord const* s = findSym(model, name);
        ASSERT_NE(s, nullptr) << name;
        ASSERT_TRUE(s->type.valid()) << name;
        ASSERT_EQ(ti.kind(s->type), TypeKind::Complex) << name;
        EXPECT_EQ(ti.kind(ti.complexElement(s->type)), TypeKind::F32)
            << name << ": `<gnu complex> float` must have an F32 element";
    }
}

// (4b) The GNU complex spelling inherits the ISO spelling's LOUD invalid
// multiset: `__complex__ int` is S_InvalidTypeSpecifierCombination, exactly as
// `_Complex int` is (ComplexKeyword IS in the specifier vocabulary but
// [Complex, Int] is not a row). This is the mirror of
// ComplexIntAndImaginaryFailLoud, and it is the pin that separates
// ComplexKeyword from the adjacent ImaginaryKeyword: an `__complex__` mis-keyed
// to ImaginaryKeyword would give S_UnknownType here, not this code.
TEST(SemanticAnalyzerC, GnuComplexSpellingsInheritInvalidMultisetDiagnostic) {
    for (std::string_view spelling : {"__complex__", "__complex"}) {
        auto model = analyzeShipped(
            "c",
            {"int main(void){ " + std::string(spelling) + " int y; return 0; }\n"});
        EXPECT_TRUE(model.hasErrors()) << spelling;
        EXPECT_GT(countCode(model.diagnostics(),
                            DiagnosticCode::S_InvalidTypeSpecifierCombination),
                  0u)
            << spelling << " int` must fail with the SAME invalid-multiset code "
                           "`_Complex int` fails with — S_UnknownType here "
                           "would mean the row landed on ImaginaryKeyword";
        EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType),
                  0u)
            << spelling << ": the spelling IS a known specifier; only the "
                           "COMBINATION is invalid";
    }
}

// (5) `__restrict__` / `__restrict` — the kind with no interned bit, pinned in
// BOTH directions.
//
// ★ THE REFUSAL IS THE DISCRIMINATOR, and this is the pin that makes the
// restrict rows provable at all. `headQualifier` is {Const, Volatile, Atomic}
// and `typeSpecifierSeq` admits Signed/Complex, so a bare qualifier in front of
// a base type is ACCEPTED for every other kind these rows could plausibly have
// landed on — and REFUSED for RestrictKeyword. MEASURED: the P0009 for both
// `restrict int x;` and `__restrict__ int x;` lists ConstKeyword,
// VolatileKeyword, AtomicKeyword, SignedKeyword and ComplexKeyword among the
// tokens it WOULD have accepted there. So "the GNU spelling is rejected in head
// position, exactly like ISO `restrict`" is a live, falsifiable assertion that
// only the correct mapping satisfies.
TEST(SemanticAnalyzerC, GnuRestrictSpellingsMatchIsoIncludingWhereRefused) {
    // (a) ACCEPTED where `restrict` is accepted: after a `*` (pointer
    // qualifier), on a parameter, twice in one signature, and in the
    // array-suffix modifier slot. Same interned types as the ISO spelling.
    auto cu = buildShippedUnit("c", {
        "int * restrict pIso;\n"
        "int * __restrict__ pGnuLong;\n"
        "int * __restrict pGnuShort;\n"
        "void fIso(int * restrict a, int * restrict b);\n"
        "void fGnuLong(int * __restrict__ a, int * __restrict__ b);\n"
        "void fGnuShort(int * __restrict a, int * __restrict b);\n"
        "void fMixed(int * __restrict__ a, int * __restrict b);\n"
        "void aIso(int a[restrict 4]);\n"
        "void aGnuLong(int a[__restrict__ 4]);\n"
        "void aGnuShort(int a[__restrict 4]);\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors())
        << "every position ISO `restrict` is legal in must accept both GNU "
           "spellings — including the array-suffix modifier slot";
    auto const& ti = model.lattice().interner();
    SymbolRecord const* iso = findSym(model, "pIso");
    ASSERT_NE(iso, nullptr);
    ASSERT_TRUE(iso->type.valid());
    ASSERT_EQ(ti.kind(iso->type), TypeKind::Ptr);
    for (char const* name : {"pGnuLong", "pGnuShort"}) {
        SymbolRecord const* s = findSym(model, name);
        ASSERT_NE(s, nullptr) << name;
        ASSERT_TRUE(s->type.valid()) << name;
        EXPECT_EQ(s->type.v, iso->type.v)
            << name << ": a GNU restrict spelling must intern to the SAME type "
                       "`int * restrict` interns to";
        // restrict is NOT an interned qualifier (only VolatileQual is), so a
        // row mis-keyed to VolatileKeyword would set the volatile bit here.
        EXPECT_FALSE(ti.isVolatileQualified(s->type))
            << name << ": restrict has no interned bit — a volatile bit here "
                       "means the row landed on VolatileKeyword";
    }

    // (b) REFUSED where `restrict` is refused — the discriminating half. Both
    // GNU spellings must fail in head position, and they must fail the SAME WAY
    // ISO `restrict` fails: a BUILDER (parse) error, which is why this counts
    // tree diagnostics rather than semantic ones.
    auto const headPositionErrors = [](std::string_view spelling) -> bool {
        auto cu2 = buildShippedUnit(
            "c", {std::string(spelling) + " int headQualified;\n"});
        return countBuilderErrors(*cu2) > 0u;
    };
    ASSERT_TRUE(headPositionErrors("restrict"))
        << "instrument check: ISO `restrict int x;` must ALREADY be refused in "
           "head position (headQualifier is {Const, Volatile, Atomic}); if it "
           "were accepted, the assertions below would prove nothing";
    for (std::string_view spelling : {"__restrict__", "__restrict"}) {
        EXPECT_TRUE(headPositionErrors(spelling))
            << spelling << " must be REFUSED in declaration-head position, "
                           "exactly as ISO `restrict` is. Accepting it here is "
                           "the signature of a row mis-keyed to Const/Volatile/"
                           "Atomic/Signed/Complex — every one of which IS legal "
                           "in this slot, so this refusal is what pins the row "
                           "to RestrictKeyword";
    }
    // NEGATIVE control for (b): a kind that IS legal in head position is
    // accepted there, proving the helper detects acceptance and is not simply
    // reporting an error for every input.
    EXPECT_FALSE(headPositionErrors("const"))
        << "`const int x;` must be ACCEPTED in head position — otherwise the "
           "refusal assertions above are vacuous";
    EXPECT_FALSE(headPositionErrors("__const__"))
        << "`__const__ int x;` must be ACCEPTED in head position — the GNU "
           "const alias reaches headQualifier";
}

// (6) THE SIDE EFFECT THAT IS A FEATURE, pinned so it cannot silently regress:
// `__volatile__` reaching VolatileKeyword means `asmStmt`'s qualifier slot
// accepts `__asm__ __volatile__ ("")` — the form real headers write. That closes
// the `__volatile__` half of D-CSUBSET-INLINE-ASM-SPELLING. Bare `asm` stays
// deliberately absent (it is an ordinary identifier in standard C), and the `:`
// OPERAND LIST is a separate open gap — so the second half of this test pins the
// RESIDUE as still LOUD, which is what keeps the half-closure from being read as
// a whole one.
//
// ★★ THE INTENT OF THE SECOND HALF IS UNCHANGED — THE TIER IT MEASURES MOVED,
// AND THIS PARAGRAPH EXISTS SO THE EDIT IS NOT READ AS A WEAKENING (updated
// 2026-08-12, inline-asm P1). It used to assert `countBuilderErrors(*cu) > 0`,
// i.e. "the `rdtsc` shape produces a PARSE error". P1 moved the refusal from the
// parse tier to the SEMANTIC tier BY DESIGN: `asmStmt` now admits the full GNU
// extended form (`asm.lang.json`, reached through `languageReferences`) precisely so
// the compiler can say WHICH construct it cannot support instead of dying at the
// first `:` — the parse error was measured to never recover, burying every later
// error in the file. So the PARSE count is now legitimately 0, and reading that
// 0 as "it still fails loud" would be exactly the accept-and-ignore this test was
// written to forbid. The assertion below is therefore STRICTER, not looser: it
// no longer accepts "some parse error, any number of them", it demands EXACTLY
// ONE diagnostic and demands it be `S_InlineAsmExtendedUnsupported`. A silent
// acceptance still fails this test — at the tier where the refusal now lives.
TEST(SemanticAnalyzerC, GnuVolatileSpellingReachesInlineAsmQualifierSlot) {
    for (std::string_view qual : {"volatile", "__volatile__", "__volatile"}) {
        auto cu = buildShippedUnit(
            "c", {"int main(void){ __asm__ " + std::string(qual) +
                         " (\"\"); return 0; }\n"});
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_FALSE(model.hasErrors())
            << qual << ": the empty-template barrier must be accepted with "
                       "this qualifier spelling";
    }
    // ★★★ THE RESIDUE IS GONE, AND THAT IS THE HEADLINE OF INLINE-ASM P5.
    // This block used to assert that sqlite's `src/hwtime.h` shape still
    // failed loud, on the grounds that accepting an operand list without
    // BINDING it would be a miscompile. That reasoning was right and it is now
    // satisfied the other way: P5 CAPTURES both operands — constraint,
    // symbolic name and value-expression NodeId — into the HIR descriptor, so
    // there is nothing being dropped and nothing left to refuse.
    // ⚠ A REFUSAL HERE IS NOW THE REGRESSION. If S0062 comes back for this
    // shape, the front end has stopped capturing and the arc's exit criterion
    // has gone with it.
    auto cu = buildShippedUnit("c", {
        "unsigned long long hw(void){ unsigned int lo, hi;\n"
        "  __asm__ __volatile__ (\"rdtsc\" : \"=a\" (lo), \"=d\" (hi));\n"
        "  return (unsigned long long)hi << 32 | lo; }\n",
    });
    EXPECT_EQ(countBuilderErrors(*cu), 0u)
        << "P1 admits the extended form at the PARSE tier on purpose — a parse "
           "error here would be the pre-P1 unrecovered cascade coming back";
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InlineAsmExtendedUnsupported), 0u)
        << "sqlite's `src/hwtime.h` shape is what P5 exists to compile";
    EXPECT_EQ(model.diagnostics().all().size(), 0u)
        << "and it must cost NO messages at all — no cascade, no residual "
           "refusal, no second opinion about the same statement";
}

// ═══════════════════════════════════════════════════════════════════════════
// inline-asm P1 (D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED): the accept /
// refuse PARITY surface, and the headline "one diagnostic, parser recovers"
// property. The asm grammar itself lives in `src/dss-config/sources/asm.lang.json`
// and reaches c through `languageReferences`; these pins are written
// against the OBSERVABLE behaviour (diagnostic code + count + message content +
// which symbols survived), never against a rule name, so moving a rule between
// the two documents cannot silently break or silently satisfy them.
//
// ★ WHY MESSAGE CONTENT IS ASSERTED AND NOT JUST THE CODE. P1's stated bonus is
// that it INVENTORIES the constraint strings it refuses — that inventory is the
// data phase P5 needs to scope real operand binding, and it is emitted only from
// the extended-asm path. A count-only pin stays green if the inventory silently
// stops being gathered; asserting the quoted text keeps the payload walk honest.
namespace {
// The `actual` text of the FIRST diagnostic carrying `code`, or "" when the code
// is absent. Content assertions read through this so a missing code fails with a
// readable empty string rather than an out-of-range index.
[[nodiscard]] std::string
firstMessageWithCode(DiagnosticReporter const& r, DiagnosticCode code) {
    for (auto const& d : r.all())
        if (d.code == code) return d.actual;
    return {};
}
// The whole reporter's text, for a failure message that has to explain a COUNT.
[[nodiscard]] std::string allMessages(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all()) {
        out += "\n      [";
        out += diagnosticCodeName(d.code);
        out += "] ";
        out += d.actual;
    }
    return out.empty() ? std::string{"<none>"} : out;
}
} // namespace

// ★★ THE HEADLINE PIN: sqlite `src/hwtime.h`'s `rdtsc` shape costs EXACTLY ONE
// diagnostic, AND the four functions written after it are REALLY PARSED.
//
// ✔MEASURED pre-P1 (recorded in asm.lang.json's `asmStmt` $comment): the first `:`
// produced P_UnexpectedToken and the parser NEVER RECOVERED — it consumed the
// rest of the translation unit inside an unterminated paren scope, so the error
// count scaled with FILE LENGTH (53 diagnostics for this very file shape). The
// invariant P1 buys is not "fewer messages", it is that the count does NOT move
// with what follows the asm statement.
//
// ★★ THE SECOND HALF IS THE LOAD-BEARING ONE, AND IT IS WHY THIS TEST NAMES
// FUNCTIONS INSTEAD OF COUNTING SILENCE. "No further diagnostics" is satisfied
// by a parser that silently EATS the remainder of the file — the exact pre-P1
// failure, minus its error messages, which would be strictly worse than what it
// replaced. So recovery is asserted POSITIVELY: each of the four trailing
// functions must exist as a Function symbol BY NAME, and each must have minted
// its own body-local variable BY NAME, which a recovery that resynchronised at
// top level but skipped the bodies could not produce.
TEST(SemanticAnalyzerC, InlineAsmExtendedRefusalCostsOneDiagnosticAndParserRecovers) {
    auto cu = buildShippedUnit("c", {
        "unsigned long long hw(void){ unsigned int lo, hi;\n"
        "  __asm__ __volatile__ (\"rdtsc\" : \"=a\" (lo), \"=d\" (hi));\n"
        "  return (unsigned long long)hi << 32 | lo; }\n"
        "int afterOne(int a){ int localOne = a + 1; return localOne; }\n"
        "int afterTwo(int a){ int localTwo = a + 2; return localTwo; }\n"
        "int afterThree(int a){ int localThree = a + 3; return localThree; }\n"
        "int afterFour(int a){ int localFour = a + 4; return localFour; }\n"
        "int main(void){ return afterOne(1)+afterTwo(2)+afterThree(3)+afterFour(4); }\n",
    });
    // (a) the PARSE tier is silent — P1 admits the construct so it can refuse it
    //     precisely. A parse error here is the unrecovered cascade returning.
    EXPECT_EQ(countBuilderErrors(*cu), 0u)
        << "the extended form must PARSE; the refusal is semantic";

    auto model = analyze(cu, DiagnosticBudget::libraryDefault());

    // (b) ⚠⚠ THE VERDICT FLIPPED IN INLINE-ASM P5 AND THE TEST FLIPPED WITH IT.
    // This used to assert exactly ONE S_InlineAsmExtendedUnsupported. `rdtsc`
    // with `"=a"(lo), "=d"(hi)` is sqlite's own `src/hwtime.h` and is the
    // arc's exit criterion — P5 CAPTURES it (both operands, both value
    // expressions, into the HIR descriptor) instead of refusing it, so ZERO
    // diagnostics is now the correct answer.
    // ★ THE COUNT ASSERTION IS THE PART THAT SURVIVES UNCHANGED IN SPIRIT: the
    // property was never "one refusal", it was "this construct costs a BOUNDED
    // number of messages that does not scale with file length" — the pre-P1
    // cascade produced 53 for this shape. Zero satisfies that strictly better.
    EXPECT_EQ(model.diagnostics().all().size(), 0u)
        << "P5 binds this statement; it must cost NO messages, and in "
           "particular the count must not scale with the four functions written "
           "after it (the pre-P1 cascade produced 53). Got: "
        << allMessages(model.diagnostics());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InlineAsmExtendedUnsupported), 0u)
        << "S0062 is the RESIDUAL refusal now — it fires only when the CAPTURE "
           "itself could not complete, never on a shape that captured cleanly. "
           "Got: " << allMessages(model.diagnostics());

    // (d) ★ RECOVERY, ASSERTED POSITIVELY AND BY EXACT NAME. Both the function
    //     and a variable declared INSIDE its body: the pair distinguishes "the
    //     parser resynchronised and read the whole file" from "it resynchronised
    //     at top level and skipped every body".
    for (auto const& [fn, local] :
         std::initializer_list<std::pair<char const*, char const*>>{
             {"afterOne", "localOne"}, {"afterTwo", "localTwo"},
             {"afterThree", "localThree"}, {"afterFour", "localFour"}}) {
        SymbolRecord const* f = findSym(model, fn);
        EXPECT_NE(f, nullptr) << fn << " was not parsed at all — the asm "
                                 "statement swallowed the rest of the file";
        if (f != nullptr) EXPECT_EQ(f->kind, DeclarationKind::Function) << fn;
        SymbolRecord const* v = findSym(model, local);
        EXPECT_NE(v, nullptr) << local << ": " << fn << "'s BODY was not parsed "
                                 "— recovery resynchronised at top level only";
        if (v != nullptr) EXPECT_EQ(v->kind, DeclarationKind::Variable) << local;
    }
    // `main` and the asm-carrying function itself round out the file.
    EXPECT_NE(findSym(model, "main"), nullptr);
    EXPECT_NE(findSym(model, "hw"), nullptr);
}

// ── ACCEPT parity: the section-bearing-but-EMPTY forms ─────────────────────
// ✔MEASURED 2026-08-12 on gcc/clang: every form below is not merely accepted but
// RUN — they are ordinary empty optimizer barriers that happen to be written
// with separators. An empty section produces NO list node (each payload rule is
// non-nullable), so "section present" and "payload present" are different
// questions and only the second one refuses. Refusing these would be the TF-C77
// failure mode: a gate that rejects code every real toolchain compiles.
// ⚠ `("" :: )` and `("" ::: )` arrive as the FUSED `ColonColonOp` token by
// maximal munch, so this group also covers the fused tail rules, not just the
// plain ones.
// ★★ EACH ACCEPTED FORM CARRIES A POSITIVE ANCHOR, AND WITHOUT ONE THIS TEST IS
// SATISFIED BY THE GATE NEVER RUNNING. `diagnostics().all().size() == 0` is a
// pure absence claim: it stays green if the inline-asm tier stops visiting the
// asm statement entirely, if the semantic facet vanishes from the config, or if
// these forms stop reaching the analyzer at all — i.e. through exactly the
// regressions that would silently accept an asm nobody checked. So every form is
// ALSO fed with a NON-EMPTY template, which must land on
// S_InlineAsmNonEmptyTemplate exactly once. Read as a pair: the gate DOES look at
// this shape (the mutant refuses), and looking at it, it says nothing (the
// original is clean). Neither half is the property on its own.
TEST(SemanticAnalyzerC, InlineAsmEmptySectionFormsAreAccepted) {
    for (std::string_view form : {"(\"\")", "(\"\" : : )", "(\"\" : : : )",
                                  "(\"\" :: )", "(\"\" ::: )"}) {
        auto cu = buildShippedUnit(
            "c",
            {"int main(void){ __asm__ " + std::string(form) + "; return 0; }\n"});
        EXPECT_EQ(countBuilderErrors(*cu), 0u) << form << ": must PARSE";
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_EQ(model.diagnostics().all().size(), 0u)
            << form << ": accepted by gcc and clang and RUN — DSS must emit "
                       "nothing at all. Got: " << allMessages(model.diagnostics());

        // THE ANCHOR, WITHOUT WHICH THE ZERO-DIAGNOSTIC CLAIM ABOVE IS VACUOUS.
        // ⚠ IT HAD TO CHANGE IN P5: it used to be `"hlt"` earning
        // S_InlineAsmNonEmptyTemplate, and a non-empty template is now
        // ACCEPTED, so that anchor would have gone quietly inert — a pin whose
        // mutant stopped being a mutant, which is precisely the vacuity this
        // project keeps catching. The replacement is a template that is still
        // refused for a reason PROPERTY OF THIS SHAPE: `%0` in a statement with
        // no operands. If the analyzer ever stops visiting these forms, the
        // anchor goes green and reds the test.
        std::string mutant{form};
        mutant.replace(mutant.find("\"\""), 2, "\"m %0\"");
        auto mutCu = buildShippedUnit(
            "c",
            {"int main(void){ __asm__ " + mutant + "; return 0; }\n"});
        EXPECT_EQ(countBuilderErrors(*mutCu), 0u) << mutant << ": must PARSE";
        auto mutModel = analyze(mutCu, DiagnosticBudget::libraryDefault());
        EXPECT_EQ(countCode(mutModel.diagnostics(),
                            DiagnosticCode::S_InlineAsmPlaceholderOutOfRange)
                      + countCode(mutModel.diagnostics(),
                                  DiagnosticCode::S_InlineAsmPlaceholderInBasicTemplate),
                  1u)
            << mutant << ": the inline-asm gate must VISIT this shape — "
                         "otherwise the zero-diagnostic assertion above is "
                         "satisfied by nothing running. Got: "
            << allMessages(mutModel.diagnostics());
    }
    // The GNU dunder qualifier spelling on the same accepted shape.
    auto cu = buildShippedUnit(
        "c", {"int main(void){ __asm__ __volatile__ (\"\"); return 0; }\n"});
    EXPECT_EQ(countBuilderErrors(*cu), 0u);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(model.diagnostics().all().size(), 0u)
        << "`__asm__ __volatile__ (\"\")` is the form real headers write. Got: "
        << allMessages(model.diagnostics());
    // ...and its anchor, for the same reason and with the same P5 change.
    auto dunderMut = buildShippedUnit(
        "c",
        {"int main(void){ __asm__ __volatile__ (\"m %0\"); return 0; }\n"});
    EXPECT_EQ(countBuilderErrors(*dunderMut), 0u);
    auto dunderModel = analyze(dunderMut, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(dunderModel.diagnostics(),
                        DiagnosticCode::S_InlineAsmPlaceholderInBasicTemplate), 1u)
        << "the dunder-qualifier shape must reach the gate too. Got: "
        << allMessages(dunderModel.diagnostics());
}

// ── REJECT parity (1): a label section without `goto` ──────────────────────
// ✔MEASURED 2026-08-12: gcc 13.3.0, clang 18.1.3 and clang 19.1.1 all reject
// these. It is a CONSTRAINT VIOLATION, not a not-yet-supported deferral — still
// ill-formed after P5 lands — which is why it has its own code AHEAD of
// S_InlineAsmExtendedUnsupported and must not degrade into it.
// ★★ FOUR SPELLINGS, THREE TOKEN ROUTES — AND THE ROUTE IS MEASURED HERE, NOT
// DESCRIBED IN PROSE. An earlier revision of this comment claimed "the three
// spellings reach the fourth section by three different token routes (all-fused,
// plain+fused, fused+fused), so each exercises a different tail rule". ✔MEASURED
// FALSE 2026-08-12: `("" ::::)` and `("" :: ::)` are the SAME token pair
// (`::`, `::`) — whitespace BETWEEN two fused pairs is lexically irrelevant, so
// those two share ONE route — and the loop was missing the ALL-PLAIN form
// altogether, which is the only spelling that reaches `asmLabelsTail` unfused.
// Two routes claimed as three, with the third one absent:
// [[D-CONFIG-COMMENT-CLAIM-ROT]] in test prose. The four forms and their routes now are
//     ("" ::::)      ::  ::         asmInputsTailFused → asmLabelsTailFused
//     ("" :: ::)     ::  ::         the SAME route — kept as the WHITESPACE
//                                   CONTROL, the pair that proves the fusion is
//                                   lexical rather than written-form
//     ("" : : ::)    :  :  ::       asmInputsTail      → asmLabelsTailFused
//     ("" : : : : )  :  :  :  :     asmInputsTail      → asmLabelsTail
// and each row's route is CHECKED AGAINST THE CST below, so the claim cannot rot
// again: a tokenizer or grammar change that re-routes a form goes red here
// rather than quietly making a sentence false.
// ⓘ ON THE ADDED ALL-PLAIN FORM AND THE REFERENCE COMPILERS: the ✔MEASURED note
// above is over the CONSTRAINT — a fourth section with no `goto` — not over a
// spelling, and the all-plain form violates exactly that constraint. Its DSS
// behaviour is measured right here; no new gcc/clang run is claimed for it.
TEST(SemanticAnalyzerC, InlineAsmLabelSectionWithoutGotoIsRefused) {
    struct Form {
        char const* text;
        bool        inputsFused;   // the SECOND section was reached by a `::`
        bool        labelsFused;   // the FOURTH section was reached by a `::`
    };
    for (auto const& form : std::initializer_list<Form>{
             {"(\"\" ::::)",     true,  true},
             {"(\"\" :: ::)",    true,  true},
             {"(\"\" : : ::)",   false, true},
             {"(\"\" : : : : )", false, false}}) {
        auto cu = buildShippedUnit(
            "c",
            {"int main(void){ __asm__ " + std::string(form.text) + "; return 0; }\n"});
        EXPECT_EQ(countBuilderErrors(*cu), 0u) << form.text << ": must PARSE";

        // THE ROUTE, off the CST. `asmInputsTail`/`asmLabelsTail` are opened by
        // a PLAIN separator; their `…Fused` siblings are the sections a single
        // `::` jumped into. Which of each pair minted a node IS the route.
        Tree const& tree = cu->trees()[0];
        RuleId const inputsPlainRule = tree.schema().rules().find("asmInputsTail");
        RuleId const inputsFusedRule = tree.schema().rules().find("asmInputsTailFused");
        RuleId const labelsPlainRule = tree.schema().rules().find("asmLabelsTail");
        RuleId const labelsFusedRule = tree.schema().rules().find("asmLabelsTailFused");
        ASSERT_TRUE(inputsPlainRule.valid() && inputsFusedRule.valid()
                    && labelsPlainRule.valid() && labelsFusedRule.valid())
            << "the four tail rules must reach c through asm's "
               "languageReferences merge — without them this measurement is "
               "vacuous rather than false";
        bool sawInputsPlain = false, sawInputsFused = false;
        bool sawLabelsPlain = false, sawLabelsFused = false;
        walkPreOrder(tree, [&](TreeCursor const& cursor) {
            NodeId const n = cursor.current();
            if (tree.kind(n) != NodeKind::Internal) return;
            auto const r = tree.rule(n).v;
            if (r == inputsPlainRule.v) sawInputsPlain = true;
            if (r == inputsFusedRule.v) sawInputsFused = true;
            if (r == labelsPlainRule.v) sawLabelsPlain = true;
            if (r == labelsFusedRule.v) sawLabelsFused = true;
        });
        EXPECT_EQ(sawInputsFused,  form.inputsFused)
            << form.text << ": inputs boundary took the wrong arm";
        EXPECT_EQ(sawInputsPlain, !form.inputsFused)
            << form.text << ": inputs boundary took the wrong arm";
        EXPECT_EQ(sawLabelsFused,  form.labelsFused)
            << form.text << ": labels boundary took the wrong arm — the "
                            "all-plain form is the ONLY one that may reach "
                            "asmLabelsTail unfused";
        EXPECT_EQ(sawLabelsPlain, !form.labelsFused)
            << form.text << ": labels boundary took the wrong arm";

        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_InlineAsmLabelSectionRequiresGoto), 1u)
            << form.text << ": a fourth `:` group without `goto` is ill-formed "
                            "in gcc, clang and MSVC alike. Got: "
            << allMessages(model.diagnostics());
        EXPECT_EQ(model.diagnostics().all().size(), 1u)
            << form.text << ": exactly one message — it must NOT also report "
                            "the generic extended-asm deferral for the same "
                            "statement";
    }
    // NEGATIVE CONTROL — without it the three assertions above are satisfiable by
    // a gate that fires on any asm at all. `asm goto` with the SAME fourth
    // section and a real label is NOT a constraint violation, so S0063 must NOT
    // fire on it.
    // ⚠ THE ANCHOR HALF HAD TO CHANGE IN P5, AND THE REASON IS THE SAME ONE
    // THAT MADE THIS TEST WORTH KEEPING. It used to anchor on
    // S_InlineAsmExtendedUnsupported firing once and inventorying `labels: lbl`
    // / `qualifier: goto`. `asm goto` is now ACCEPTED (operator ruling: follow
    // clang), so that anchor would have gone quietly inert — a mutant that
    // stopped being a mutant, which is exactly the vacuity the anchor exists to
    // prevent. The replacement anchors on a refusal that is a property of THIS
    // shape and still stands: `%0` in a statement whose operand count is zero.
    auto cu = buildShippedUnit("c", {
        "int main(void){ __asm__ goto (\"m %0\" : : : : lbl); lbl: return 0; }\n"});
    EXPECT_EQ(countBuilderErrors(*cu), 0u);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InlineAsmLabelSectionRequiresGoto), 0u)
        << "WITH `goto` the label section is legal — reporting the constraint "
           "violation here would refuse valid C";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InlineAsmPlaceholderOutOfRange), 1u)
        << "the POSITIVE anchor: the inline-asm tier must VISIT this shape. Got: "
        << allMessages(model.diagnostics());

    // ...and the LABEL and QUALIFIER facts, asserted against the CAPTURE rather
    // than against a refusal message that no longer exists. Strictly stronger:
    // the old form could only observe them through the text of a diagnostic.
    auto plain = buildShippedUnit("c", {
        "int main(void){ __asm__ goto (\"\" : : : : lbl); lbl: return 0; }\n"});
    auto plainModel = analyze(plain, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(plainModel.hasErrors())
        << "`asm goto` with a real label is accepted by clang 18/19 and the "
           "operator ruled to follow clang. Got: "
        << allMessages(plainModel.diagnostics());
    auto const& gia = plain->schema().semantics().inlineAsm;
    bool sawGoto = false;
    for (auto const& tr : plain->trees()) {
        walkPreOrder(tr, [&](TreeCursor const& cursor) {
            NodeId const n = cursor.current();
            if (tr.kind(n) != NodeKind::Internal) return;
            if (!gia.rule.valid() || tr.rule(n).v != gia.rule.v) return;
            sawGoto = true;
            auto const f = gatherInlineAsmFacts(
                tr, n, gia, plain->schema().semantics().inlineAsmTemplateLexemes,
                plain->schema().semantics().identifierToken,
                plain->schema().hirLowering().stringBodyToken);
            EXPECT_TRUE(f.hasGotoQualifier);
            EXPECT_EQ(f.gotoQualifierText, "goto");
            ASSERT_EQ(f.labels.size(), 1u);
            EXPECT_EQ(f.labels[0].name, "lbl");
        });
    }
    EXPECT_TRUE(sawGoto);
}

// ── REJECT parity (2): a duplicated qualifier, BY KIND not by spelling ─────
// ✔MEASURED 2026-08-12: gcc, clang and MSVC all reject a repeated qualifier.
// ★ THE SECOND AND THIRD CASES ARE THE INTERESTING ONES. `volatile __volatile__`
// is two DIFFERENT SPELLINGS of ONE qualifier — DSS's keyword table aliases the
// dunder form onto `VolatileKeyword` — so a spelling-based duplicate check would
// accept it. The message additionally ECHOES THE SOURCE TEXT of the offending
// token, so the reader sees what they wrote rather than a canonicalised form;
// that content assertion is what distinguishes "detected by kind" from "detected
// by kind and then reported as the wrong word".
TEST(SemanticAnalyzerC, InlineAsmDuplicateQualifierIsRefusedByKindNotSpelling) {
    for (auto const& [quals, echoed] :
         std::initializer_list<std::pair<char const*, char const*>>{
             {"volatile volatile", "volatile"},
             {"volatile __volatile__", "__volatile__"},
             {"__volatile__ volatile", "volatile"}}) {
        auto cu = buildShippedUnit(
            "c", {"int main(void){ __asm__ " + std::string(quals) +
                         " (\"\"); return 0; }\n"});
        EXPECT_EQ(countBuilderErrors(*cu), 0u) << quals << ": must PARSE";
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_InlineAsmDuplicateQualifier), 1u)
            << quals << ": a repeated qualifier is rejected by gcc, clang and "
                        "MSVC alike — and `volatile __volatile__` IS repeated, "
                        "because the two spellings are one qualifier. Got: "
            << allMessages(model.diagnostics());
        EXPECT_EQ(model.diagnostics().all().size(), 1u)
            << quals << ": exactly one message";
        std::string const msg = firstMessageWithCode(
            model.diagnostics(), DiagnosticCode::S_InlineAsmDuplicateQualifier);
        EXPECT_NE(msg.find(std::string{"`"} + echoed + "`"), std::string::npos)
            << quals << ": the message must echo the SECOND occurrence's own "
                        "source text, got: " << msg;
    }
    // NEGATIVE CONTROL: two DIFFERENT qualifiers are not a duplicate. Without
    // this, a check that fired on "two qualifier tokens" would pass above.
    //
    // ★★ AND IT CARRIES A POSITIVE ANCHOR, WHICH IT DID NOT BEFORE. Asserting
    // ONLY `countBuilderErrors == 0` and `DuplicateQualifier == 0` is satisfied
    // by a qualifier scan that NEVER RAN — on this statement, on this shape, or
    // at all. "The duplicate check did not misfire" and "the duplicate check
    // exists" are different claims and the absence assertion is only the first.
    // Its twin thirty lines up (the `asm goto` control in
    // `InlineAsmLabelSectionWithoutGotoIsRefused`) gets this right by pinning
    // the OTHER code positively, and this one now does the same: the statement
    // has a fourth section and a `goto`, so it MUST land on
    // S_InlineAsmExtendedUnsupported exactly once, with the label inventoried.
    // If the inline-asm tier ever stops visiting this shape, that arm goes red
    // where the two zeros below would stay green.
    // ⚠ SAME P5 ANCHOR CHANGE as its twin thirty lines up, for the same reason:
    // `asm goto` is accepted now, so anchoring on S_InlineAsmExtendedUnsupported
    // would assert nothing. `%0` with zero operands is a refusal that IS a
    // property of this shape.
    auto cu = buildShippedUnit(
        "c", {"int main(void){ __asm__ goto volatile (\"m %0\" : : : : l); "
                     "l: return 0; }\n"});
    EXPECT_EQ(countBuilderErrors(*cu), 0u);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InlineAsmDuplicateQualifier), 0u)
        << "`goto volatile` is two distinct qualifiers in a free-order run — "
           "not a duplicate. Got: " << allMessages(model.diagnostics());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InlineAsmPlaceholderOutOfRange), 1u)
        << "the POSITIVE anchor: this statement IS visited by the inline-asm "
           "tier, exactly once — without it the zero above is satisfied by a "
           "qualifier scan that never ran. Got: "
        << allMessages(model.diagnostics());
    EXPECT_EQ(model.diagnostics().all().size(), 1u)
        << "exactly one message. Got: " << allMessages(model.diagnostics());
}

// ── REJECT parity (3): the cycle-1 non-empty-template refusal is UNCHANGED ──
// P1 widened what PARSES without widening what LOWERS. `("hlt")` carries real
// per-target instructions; lowering it to a no-op barrier would DELETE them.
// This pins that P1's new gates did not shadow the older one — a bare non-empty
// template has no sections, so it must fall through gates (1) and (2) into (3).
// ⚠ RENAMED AND INVERTED BY P5. It was `…StillRefusedUnderP1` and asserted that
// `__asm__("hlt")` cost one S_InlineAsmNonEmptyTemplate. The template is now
// CARRIED into the HIR descriptor, so that refusal is gone — but the SECOND
// half of the old test was never about the refusal at all, and it is the half
// worth keeping: a SECTIONLESS statement must not be reported as EXTENDED asm.
// That separation is what stops 0xE057 and 0xE062 collapsing into each other,
// and it is MORE at risk now that 0xE062 has become a residual catch-all.
TEST(SemanticAnalyzerC, ASectionlessAsmIsNotReportedAsExtended) {
    auto cu = buildShippedUnit(
        "c", {"int main(void){ __asm__ (\"hlt\"); return 0; }\n"});
    EXPECT_EQ(countBuilderErrors(*cu), 0u);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InlineAsmExtendedUnsupported), 0u)
        << "a sectionless statement is not EXTENDED asm — and 0xE062 is now the "
           "RESIDUAL refusal, so it must not become the place unclassified "
           "statements land. Got: " << allMessages(model.diagnostics());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InlineAsmNonEmptyTemplate), 0u)
        << "a non-empty BASIC template is the commonest inline asm there is and "
           "every reference compiler accepts it. Got: "
        << allMessages(model.diagnostics());
    EXPECT_EQ(model.diagnostics().all().size(), 0u)
        << allMessages(model.diagnostics());
}

// ── ★ THE QUALIFIER SCAN'S BOUND: it stops at the template ─────────────────
// This pins a REAL BOUND in the implementation, and the bound is load-bearing
// rather than an optimisation. A qualifier is a plain keyword token, and an
// operand EXPRESSION may legitimately contain the same keyword —
// `"r"(*(volatile int*)&p)` is real code. An UNBOUNDED "is there a second
// `volatile` token under this asm statement" scan would see the cast's
// `volatile` and report a duplicate qualifier: i.e. REFUSE VALID C, which is
// strictly worse than the silence it replaced. The scan therefore stops at the
// template node and never enters a section, and this test is what holds it there.
//
// The statement is still refused — it has an INPUT operand — but by the
// extended-asm code, exactly once, and the message must inventory `"r"`.
TEST(SemanticAnalyzerC, InlineAsmQualifierScanStopsAtTemplateAndIgnoresOperandKeywords) {
    auto cu = buildShippedUnit("c", {
        "int main(void){ int p = 0;\n"
        "  __asm__ (\"\" : : \"r\"(*(volatile int*)&p));\n"
        "  return 0; }\n"});
    EXPECT_EQ(countBuilderErrors(*cu), 0u);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InlineAsmDuplicateQualifier), 0u)
        << "the `volatile` inside the operand's CAST is not a qualifier of the "
           "asm statement — an unbounded qualifier scan would refuse valid C "
           "here. Got: " << allMessages(model.diagnostics());
    EXPECT_EQ(model.diagnostics().all().size(), 0u)
        << "P5 captures this statement; it must cost no messages. Got: "
        << allMessages(model.diagnostics());

    // ★★ THE ROLE ASSERTIONS MOVED FROM A MESSAGE TO THE CAPTURE ITSELF, which
    // is strictly stronger. They used to read the refusal's inventory text
    // (`inputs: "r"`, and NO `outputs:` / `clobbers:`) — there is no refusal
    // now, and a test that only checked "zero diagnostics" would have silently
    // stopped asserting that the `volatile` cast is read as an INPUT operand
    // rather than as an output, a clobber, or a qualifier. So the same three
    // claims are made against `gatherInlineAsmFacts` directly.
    auto const& ia = cu->schema().semantics().inlineAsm;
    bool checked = false;
    for (auto const& tree : cu->trees()) {
        walkPreOrder(tree, [&](TreeCursor const& cursor) {
            NodeId const n = cursor.current();
            if (tree.kind(n) != NodeKind::Internal) return;
            if (!ia.rule.valid() || tree.rule(n).v != ia.rule.v) return;
            checked = true;
            auto const f = gatherInlineAsmFacts(
                tree, n, ia, cu->schema().semantics().inlineAsmTemplateLexemes,
                cu->schema().semantics().identifierToken,
                cu->schema().hirLowering().stringBodyToken);
            ASSERT_EQ(f.operands.size(), 1u);
            EXPECT_EQ(f.operands[0].constraint, "r");
            EXPECT_FALSE(f.operands[0].isOutput)
                << "the operand is in the INPUTS section";
            EXPECT_EQ(f.outputCount, 0u);
            EXPECT_TRUE(f.clobbers.empty())
                << "the `volatile` cast must not be mistaken for a clobber";
            EXPECT_TRUE(f.operands[0].valueExpr.valid());
        });
    }
    EXPECT_TRUE(checked) << "the fixture must contain one asmStmt";
}

// -- P34 D-CSUBSET-VLA-INITIALIZER (C23 6.7.10p4) ---------------------------
// "An entity of variable length array type shall not be initialized except by
// an empty initializer." ONE root cause put DSS on the wrong side of this in
// BOTH directions, and the pins below are split so a partial regression cannot
// hide behind a passing sibling.
//
// The root cause: `applyDeclaratorSuffix` tested the flexible-array flag ABOVE
// its VLA arm, and the declarator loop handed that parameter the OR of the
// row's own `allowFlexibleArray` and "this declarator has an initializer". So a
// PRESENT-but-non-constant bound with an initializer was read as an ABSENT one
// -> incomplete array -> re-sized from the brace list. The two signals now
// travel separately and the init-inference half is gated on the bound being
// ABSENT, which is what C 6.7.9p22 actually says.

// REFUSING PIN. All three references refuse this (gcc 13.3.0, clang 19.1.1,
// clang 18.1.3 -- MEASURED 2026-08-25, both -std=gnu17 and -std=c2x). DSS used
// to ACCEPT it at a compile-time sizeof of 12.
// RED-ON-DISABLE: re-merge the two signals at the declarator loop and this
// compiles clean with no diagnostic at all.
TEST(SemanticAnalyzerC, VlaWithNonEmptyInitializerIsRefused) {
    auto model = analyzeShipped("c", {
        "int main(int argc, char **argv) {\n"
        "    int a[argc] = {1, 2, 3};\n"
        "    return a[0];\n"
        "}\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_VlaInitializerNotEmpty), 1u)
        << "a variably modified object may not carry a non-empty initializer";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayLengthOutOfRange), 0u)
        << "and it must not be reported as a length problem -- the bound is fine";
}

// The constraint is on a VARIABLY MODIFIED entity, not on a top-level VLA:
// `int a[n][2]` interns as vlaArray(array(int,2)) and `int a[5][n]` as
// array(vlaArray(int),5). Both references refuse both forms.
// RED-ON-DISABLE: narrow the guard from `typeContainsVla` to `isVlaArray` and
// the second declarator below stops being reported.
TEST(SemanticAnalyzerC, VlaInitializerConstraintIsTransitiveThroughArrayLevels) {
    auto model = analyzeShipped("c", {
        "int main(int argc, char **argv) {\n"
        "    int a[argc][2] = {{1, 2}};\n"
        "    int b[5][argc] = {{1}};\n"
        "    return a[0][0] + b[0][0];\n"
        "}\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_VlaInitializerNotEmpty), 2u)
        << "an outer-VLA and an inner-VLA multi-dimensional form are both "
           "variably modified, and both are refused by gcc and clang";
}

// ACCEPTING PIN -- the half that was a REJECTION before. `int a[n] = {};` is the
// one initializer 6.7.10p4 permits, and gcc 13.3.0, clang 19.1.1 and clang
// 18.1.3 all accept it. DSS answered S000C, because the same dropped-bound path
// re-sized the object to ZERO elements and then rejected the zero.
// The type assertion is the load-bearing half: accepting the FORM while
// silently keeping a fixed-size type would still be the original bug.
TEST(SemanticAnalyzerC, VlaWithEmptyInitializerIsAcceptedAndStaysAVla) {
    auto model = analyzeShipped("c", {
        "int main(int argc, char **argv) {\n"
        "    int a[argc] = {};\n"
        "    return a[0];\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "the EMPTY initializer is the form C23 6.7.10p4 REQUIRES to be legal";
    auto const* a = findSymbolNamed(model, "a");
    ASSERT_NE(a, nullptr);
    ASSERT_TRUE(a->type.valid());
    TypeInterner const& in = model.lattice().interner();
    EXPECT_TRUE(in.isVlaArray(a->type))
        << "the written `argc` bound must survive the initializer -- an accepted "
           "form whose type quietly became int[0] or int[3] is the same defect";
    EXPECT_FALSE(in.isIncompleteArray(a->type))
        << "and it must not be the incomplete array the merged flag produced";
}

// THE THREE NEIGHBOURS THE UN-MERGE COULD HAVE BROKEN, stated separately
// because each rides a DIFFERENT one of the two signals.
//
// (1) c34 init-inference (D-CSUBSET-ARRAY-SIZE-INFERENCE, C 6.7.9p22): an
// ABSENT bound with an initializer is still sized from it. This is the arm the
// gate narrowed, so it is the one most likely to have been narrowed too far.
TEST(SemanticAnalyzerC, InitInferredArraySizingSurvivesTheVlaUnmerge) {
    auto model = analyzeShipped("c", {
        "int main(void) {\n"
        "    int a[] = {1, 2, 3};\n"
        "    char s[] = \"abc\";\n"
        "    return a[0] + (int)sizeof(s);\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    TypeInterner const& in = model.lattice().interner();
    auto const* a = findSymbolNamed(model, "a");
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(in.kind(a->type), TypeKind::Array);
    EXPECT_EQ(in.scalars(a->type)[0], 3)
        << "a brace list still sizes an absent bound";
    auto const* s = findSymbolNamed(model, "s");
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(in.kind(s->type), TypeKind::Array);
    EXPECT_EQ(in.scalars(s->type)[0], 4)
        << "and a string literal still sizes one, body + NUL";
}

// (2) The struct-field flexible array member rides the ROW's own
// `allowFlexibleArray`, which the un-merge handed back its original meaning.
// A FAM must still be an incomplete array, and a `[0]` GNU zero-length member
// must still route to the SAME mechanism.
TEST(SemanticAnalyzerC, FlexibleArrayMemberSurvivesTheVlaUnmerge) {
    auto model = analyzeShipped("c", {
        "struct F { int n; char c[]; };\n"
        "struct Z { int n; char c[0]; };\n"
        "int main(void) { return (int)sizeof(struct F) + (int)sizeof(struct Z); }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "a flexible array member and its GNU `[0]` spelling both stay legal";
}

// (3) THE `[0]`-WITH-INITIALIZER EXCLUSION, and it is not redundant. `externDecl`
// declares `allowFlexibleArray: true` AND takes an init-declarator list, so both
// bools can be true on one declarator; without the exclusion the written `[0]`
// would be replaced by an incomplete array carrying an initializer.
// RED-ON-DISABLE: drop `!allowInitInferredArray` from the `[0]` arm and the
// extern declarator below stops being reported.
TEST(SemanticAnalyzerC, ZeroBoundWithInitializerStaysOutOfRange) {
    auto model = analyzeShipped("c", {
        "extern int g[0] = {1};\n"
        "int main(void) { int a[0] = {1}; return a[0] + g[0]; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ArrayLengthOutOfRange), 2u)
        << "a WRITTEN zero bound is never silently re-sized by its initializer";
}

// -- P34 D-CSUBSET-ATTRIBUTE-TYPE-POSITION -- the enum after-keyword slot ----
// C23 6.7.3.1 puts a tag's attribute after the composite keyword. `structSpec`
// and `unionSpec` have carried `compositeAttrLead` since TF-C73; `enumSpec` was
// the last composite row without it, so BOTH spellings were PARSE errors
// (P0009 for `[[...]]`, P0001 for `__attribute__((...))`) on a position gcc
// 13.3.0 and clang 19.1.1 both accept and both warn on (MEASURED 2026-08-25).
TEST(SemanticAnalyzerC, EnumAfterKeywordDeprecatedWarnsAtEveryUse) {
    auto model = analyzeShipped("c", {
        "enum [[deprecated]] E1 { A1 = 1 };\n"
        "enum __attribute__((deprecated)) E2 { A2 = 2 };\n"
        "int main(void) {\n"
        "    enum E1 e1 = A1;\n"
        "    enum E2 e2 = A2;\n"
        "    return (int)e1 + (int)e2;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "the slot must PARSE -- this was P0009 / P0001 before P34";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_DeprecatedSymbolUsed), 2u)
        << "one warning per deprecated TAG use, both spellings, exactly as the "
           "struct and union rows already produce";
}

// THE INDEX PIN, and it is the one that would fail SILENTLY. An always-emitted
// lead slot shifts the row's tag from visible-child 1 to 2. If the row's `name`
// index had not moved with the shape, `extractNameNode` would hand
// `anonymousNameAllowed` a non-identifier node and a TAGGED enum would bind
// ANONYMOUSLY with its tag discarded -- a program using only the enumerators
// would still compile. Naming the tag in a TYPE position is what catches it.
TEST(SemanticAnalyzerC, EnumTagStillBindsWithAndWithoutTheLeadDecoration) {
    auto model = analyzeShipped("c", {
        "enum Plain { P0 = 1 };\n"
        "enum [[deprecated]] Decorated { D0 = 2 };\n"
        "enum [[deprecated]] Underlying : unsigned char { U0 = 3 };\n"
        "enum [[deprecated]] { ANON0 = 4 };\n"
        "int main(void) {\n"
        "    enum Plain p = P0;\n"
        "    enum Decorated d = D0;\n"
        "    enum Underlying u = U0;\n"
        "    return (int)p + (int)d + (int)u + ANON0;\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "an UNdecorated enum, a decorated one, a decorated one carrying the "
           "C23 underlying-type clause, and a decorated ANONYMOUS one must all "
           "keep binding -- the lead slot makes the tag index constant, not shifted";
    EXPECT_NE(findSymbolNamed(model, "Decorated"), nullptr)
        << "the decorated tag binds under its own NAME, not anonymously";
}

// The layout attributes are ACCEPTED (both references compile them) and
// reported IGNORED rather than silently dropped -- DSS's enum width comes from
// its underlying type, which neither attribute feeds. Admitting the slot
// without this arm would have traded a loud parse error for a quiet wrong
// layout, which is the exact trade `compositeAttrLead`'s design note forbids.
TEST(SemanticAnalyzerC, EnumLayoutAttributeIsAcceptedAndReportedIgnored) {
    auto model = analyzeShipped("c", {
        "enum __attribute__((packed)) P1 { B1 = 1 };\n"
        "enum __attribute__((aligned(16))) P2 { B2 = 2 };\n"
        "int main(void) { enum P1 a = B1; enum P2 b = B2; return (int)a + (int)b; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "gcc and clang both accept these -- DSS may not refuse them";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AttributeIgnoredForDeclarationKind), 2u)
        << "one report per ignored layout attribute -- never silence";
}

// The strict-unknown typo guard the row opted into reaches the enum slot too:
// a misspelled GNU attribute there must fail loud rather than leave the tag
// quietly undecorated. The C23 `[[...]]` spelling stays standard-ignorable.
TEST(SemanticAnalyzerC, EnumLeadSlotKeepsTheStrictUnknownAttributeGuard) {
    auto strict = analyzeShipped("c", {
        "enum __attribute__((deprected)) E { A = 1 };\n"
        "int main(void) { enum E e = A; return (int)e; }\n",
    });
    EXPECT_EQ(countCode(strict.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 1u)
        << "a GNU-form typo on an enum fails loud, as it does on a struct";
    auto ignorable = analyzeShipped("c", {
        "enum [[vendor::whatever]] E { A = 1 };\n"
        "int main(void) { enum E e = A; return (int)e; }\n",
    });
    EXPECT_EQ(countCode(ignorable.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u)
        << "an unrecognized C23 attribute stays standard-ignorable";
}

// -- P34 D-DIAG-ARRAY-SUFFIX-REPORTS-ONLY-THE-LEXEME ------------------------
// THE DEFECT WAS VISIBLE ONLY ONCE THE LEXEME HAPPENED TO BE EMPTY-LOOKING.
// The two array-suffix `emit` lambdas and `completeIncompleteArrayFromInit`'s
// `failUnsized` put the RAW SOURCE TEXT of the offending node into `actual`, and
// `actual` is the whole rendered body for a semantic-band code (the renderer's
// own contract at `appendExpectedActual`: these codes "carry a full prose
// sentence in `actual` and leave `expected` empty by design"). So the reports
// read `error[S000C]: [0]` — and, for `int a[argc] = {};`, `error[S000C]: {}`, a
// diagnostic whose entire message is the two braces it is complaining about.
//
// The property pinned here is the one that matters and the one a `.diag` golden
// CANNOT see: those goldens record code + span only, so the message body could
// regress to a bare lexeme with every golden still green.
// RED-ON-DISABLE: restore `d.actual = std::string{tree.text(...)}` in either
// lambda and the length assertion below fails with the lexeme as its message.
TEST(SemanticAnalyzerC, ArrayDiagnosticsCarryProseNotOnlyTheOffendingLexeme) {
    auto model = analyzeShipped("c", {
        "int main(int argc, char **argv) {\n"
        "    int a[0] = {1};\n"      // S_ArrayLengthOutOfRange, lexeme "[0]"
        "    int b[] = {};\n"        // S_ArrayLengthOutOfRange, lexeme "{}"
        "    int c[argc][0];\n"      // S_ArrayLengthOutOfRange again
        "    return a[0] + b[0] + c[0][0];\n"
        "}\n",
    });
    std::size_t seen = 0;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_ArrayLengthOutOfRange
            && d.code != DiagnosticCode::S_NonConstantArrayLength) {
            continue;
        }
        ++seen;
        EXPECT_TRUE(d.expected.empty())
            << "a semantic-band code contrasts with nothing, so `expected` stays "
               "empty and `actual` is rendered alone";
        EXPECT_NE(d.actual, "[0]")
            << "the message body must not BE the offending lexeme";
        EXPECT_NE(d.actual, "{}")
            << "the case that made this class visible: a body of two braces";
        EXPECT_GT(d.actual.size(), 40u)
            << "expected a prose sentence, got: " << d.actual;
        EXPECT_NE(d.actual.find(' '), std::string::npos)
            << "expected a prose sentence, got: " << d.actual;
    }
    EXPECT_GE(seen, 3u)
        << "the fixture must actually reach the array-suffix diagnostics";
}
