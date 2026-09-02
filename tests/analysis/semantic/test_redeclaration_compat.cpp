// ── THE ONE C23 FUNCTION-REDECLARATION COMPATIBILITY ORACLE ──────────────────
//
// P44 lane a. Three registry rows, one defect at three depths, one predicate:
//   [[D-CSUBSET-SUPPRESSED-SHIPPED-ROW-SIGNATURE-UNCHECKED]] — a shipped
//     descriptor row was suppressed on a NAME match and the user's declaration
//     was never compared against it, so `#include <stdio.h>` + `int puts(double);`
//     + `puts(3.5)` compiled clean and called the real `puts` with a double in
//     xmm0 where it expects a `char *` in rcx.
//   [[D-LANG-TYPE-IDENTITY-QUALIFIER-BLIND-VS-C23-REDECL]] — DSS type identity is
//     qualifier-blind while C23 compatibility is qualifier-sensitive, so
//     `int f(const char *); int f(char *);` was accepted and (in the other
//     direction) the legal `int f(volatile int); int f(int);` was refused.
//   [[D-CSUBSET-INCOMPATIBLE-REDECL-DIAGNOSED-AT-CALL-SITE-NOT-DECLARATION]] —
//     `extern int printf();` over the include reported ARITY at the CALL instead
//     of an incompatible redeclaration at the DECLARATION, so the same program
//     with NO call compiled clean.
//
// ★★ EVERY ACCEPT/REFUSE EXPECTATION HERE IS ✔MEASURED AGAINST THE REFERENCES,
// PROBED SEPARATELY — gcc 13.3.0 (Ubuntu, LP64) and clang 18.1.3 for the elf
// column, and mingw-w64 gcc 13.2.0 (LLP64) for the pe column, because a reference
// control must match the TARGET. The bar is the DISJUNCTION: DSS must accept what
// any of them accepts, and must refuse what none of them does.
//
// ★ THE PAIRS ARE THE POINT. Every refusal sits beside the nearest ACCEPTING
// shape, because a redeclaration check fails in two opposite directions and only
// the pair separates a fix that landed from one that over-reached. Deleting the
// oracle's qualifier arm reds the refusals; widening it into "absent means
// unqualified" reds the acceptances.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/redeclaration_compat.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace fs = std::filesystem;

namespace {

// ── unit tier: the oracle over hand-built signatures ─────────────────────────

// A bare interner. CompilationUnitId 1 is the arena tag; nothing here reads it.
[[nodiscard]] TypeInterner makeInterner() {
    return TypeInterner{CompilationUnitId{1}};
}

[[nodiscard]] RedeclarationVerdict
compat(TypeInterner const& in, TypeId a, TypeId b,
       LeafComparison mode = LeafComparison::SourceVocabulary) {
    return functionRedeclarationCompatibility(in, DeclaredFunction{a, nullptr},
                                              DeclaredFunction{b, nullptr}, mode);
}

// ── e2e tier: the real `c` schema + the REAL shipped-lib descriptor corpus ───

[[nodiscard]] fs::path shippedLibsDir() {
    fs::path const dir = dss::test::configRoot() / "shippedLibs";
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        throw std::runtime_error("shipped-lib descriptor directory missing: "
                                 + dir.string());
    return dir;
}

// Analyze `src` the way the production driver does for one (format, dataModel):
// the real descriptor corpus on the system include path and the format active, so
// the goal-2 suppression + the platform-realization pass both run for real. The
// `analyzeRealTgmath` shape in test_semantic_analyzer_c.cpp, which is the only
// configuration in which a shipped row is ever suppressed.
[[nodiscard]] SemanticModel analyzeWithShipped(std::string src,
                                               ObjectFormatKind format,
                                               DataModel dataModel) {
    auto schema = loadShippedSchema("c");
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addSystemDir(shippedLibsDir());
    builder.setActiveFormat(format);
    builder.addInMemory(std::move(src), "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), dataModel,
                   std::nullopt, std::nullopt, format, "x86_64");
}

// The elf x86_64 leg (LP64) — the leg both Linux references model.
[[nodiscard]] SemanticModel analyzeElf(std::string src) {
    return analyzeWithShipped(std::move(src), ObjectFormatKind::Elf,
                              DataModel::Lp64);
}
// The pe x86_64 leg (LLP64) — where `printf` is a `synthesize` shim row rather
// than an import, so the CST→HIR shim gate is the tier that also sees it.
[[nodiscard]] SemanticModel analyzePe(std::string src) {
    return analyzeWithShipped(std::move(src), ObjectFormatKind::Pe,
                              DataModel::Llp64);
}

// User↔user redeclaration needs no descriptor corpus at all.
[[nodiscard]] SemanticModel analyzeC(std::string src) {
    return analyzeShipped("c", {std::move(src)});
}

[[nodiscard]] std::string firstMessage(SemanticModel const& m) {
    for (auto const& d : m.diagnostics().all())
        if (d.severity == DiagnosticSeverity::Error) return d.actual;
    return {};
}

}  // namespace

// ══ 1. THE ORACLE, AS A PREDICATE ════════════════════════════════════════════

// The axes, each isolated. A verdict must name the axis an author would fix, and
// name it FIRST — a "parameter 1 differs" report for a program that also has the
// wrong arity sends the reader to the wrong place.
TEST(RedeclarationCompat, EachDivergenceAxisIsNamedSeparately) {
    TypeInterner in = makeInterner();
    TypeId const i32  = in.primitive(TypeKind::I32);
    TypeId const i64  = in.primitive(TypeKind::I64);
    TypeId const f64  = in.primitive(TypeKind::F64);
    TypeId const p32  = in.pointer(i32);
    std::vector<TypeId> const one{i32};
    std::vector<TypeId> const oneOther{f64};
    std::vector<TypeId> const two{i32, i32};

    TypeId const base   = in.fnSig(one, i32, CallConv::CcSysV);
    TypeId const same   = in.fnSig(one, i32, CallConv::CcSysV);
    TypeId const arity  = in.fnSig(two, i32, CallConv::CcSysV);
    TypeId const varia  = in.fnSig(one, i32, CallConv::CcSysV, true);
    TypeId const ptype  = in.fnSig(oneOther, i32, CallConv::CcSysV);
    TypeId const rtype  = in.fnSig(one, i64, CallConv::CcSysV);
    TypeId const cc     = in.fnSig(one, i32, CallConv::CcMS64);

    EXPECT_TRUE(compat(in, base, same).compatible());
    EXPECT_EQ(compat(in, base, arity).axis,
              RedeclarationDivergence::ParameterCount);
    EXPECT_EQ(compat(in, base, varia).axis, RedeclarationDivergence::Ellipsis);
    EXPECT_EQ(compat(in, base, ptype).axis, RedeclarationDivergence::ParameterType);
    EXPECT_EQ(compat(in, base, ptype).parameterOrdinal, 1u);
    EXPECT_EQ(compat(in, base, rtype).axis, RedeclarationDivergence::ReturnType);
    EXPECT_EQ(compat(in, base, cc).axis,
              RedeclarationDivergence::CallingConvention);
    // A non-function pair is answered by the leaf relation and says so.
    EXPECT_EQ(compat(in, i32, p32).axis, RedeclarationDivergence::NotAFunction);
    EXPECT_TRUE(compat(in, p32, p32).compatible());
}

// C23 6.7.6.3p15, second sentence: a PARAMETER's own top-level qualifier is
// dropped. ✔gcc and clang both ACCEPT `int f(volatile int); int f(int);` — DSS
// refused it before this oracle, because `volatile` IS interned.
//
// ⚠ AND `_Atomic` IS NOT DROPPED, which is the standard's asymmetry and not a
// shortcut: `_Atomic T` is a distinct TYPE, not a qualified spelling of `T`.
// ✔gcc and clang both REFUSE the `_Atomic` twin. The pair is what proves the
// unqualification did not become a blanket strip.
TEST(RedeclarationCompat, TopLevelParameterVolatileIsDroppedButAtomicIsNot) {
    TypeInterner in = makeInterner();
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const vol = in.volatileQualified(i32);
    TypeId const ato = in.atomicQualified(i32);
    TypeId const plain    = in.fnSig(std::vector<TypeId>{i32}, i32, CallConv::CcSysV);
    TypeId const volatil  = in.fnSig(std::vector<TypeId>{vol}, i32, CallConv::CcSysV);
    TypeId const atomic   = in.fnSig(std::vector<TypeId>{ato}, i32, CallConv::CcSysV);

    EXPECT_TRUE(compat(in, plain, volatil).compatible())
        << "C23 6.7.6.3p15 takes a qualified parameter as its unqualified version";
    EXPECT_EQ(compat(in, plain, atomic).axis,
              RedeclarationDivergence::ParameterType)
        << "_Atomic makes a DISTINCT type, not a qualified one — both references "
           "refuse this pair";
}

// A POINTEE qualifier is NOT dropped — it is part of the pointed-to type
// (6.7.6.1p2). `volatile` rides the TypeId, so the structural walk sees it with
// no qualification claim needed.
TEST(RedeclarationCompat, PointeeVolatileIsPartOfTheType) {
    TypeInterner in = makeInterner();
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const pPlain = in.pointer(i32);
    TypeId const pVol   = in.pointer(in.volatileQualified(i32));
    TypeId const a = in.fnSig(std::vector<TypeId>{pPlain}, i32, CallConv::CcSysV);
    TypeId const b = in.fnSig(std::vector<TypeId>{pVol}, i32, CallConv::CcSysV);
    EXPECT_EQ(compat(in, a, b).axis, RedeclarationDivergence::ParameterType);
}

// The const spine, which is the ONLY thing the interner cannot carry. Bit 0 is
// the entity's own top level (dropped for a parameter, kept for the result);
// every deeper bit is a pointed-to type's qualifier and is load-bearing.
TEST(RedeclarationCompat, ConstSpineJudgesPointeesAndIgnoresParameterTopLevel) {
    TypeInterner in = makeInterner();
    TypeId const i8v = in.primitive(TypeKind::Char);
    TypeId const p   = in.pointer(i8v);
    TypeId const fn  = in.fnSig(std::vector<TypeId>{p}, in.primitive(TypeKind::I32),
                                CallConv::CcSysV);

    auto withParam = [](QualifierSpine sp) {
        DeclaredQualification q;
        q.params.push_back(sp);
        return q;
    };
    // `char *`      → 2 levels, nothing const
    // `const char *`→ 2 levels, level 1 (the pointee) const
    // `char *const` → 2 levels, level 0 (the parameter itself) const
    DeclaredQualification const plain   = withParam(QualifierSpine{0b00, 2});
    DeclaredQualification const pointee = withParam(QualifierSpine{0b10, 2});
    DeclaredQualification const topLvl  = withParam(QualifierSpine{0b01, 2});

    EXPECT_EQ(functionRedeclarationCompatibility(in, {fn, &plain}, {fn, &pointee}).axis,
              RedeclarationDivergence::ParameterQualification);
    EXPECT_EQ(functionRedeclarationCompatibility(in, {fn, &pointee}, {fn, &plain}).axis,
              RedeclarationDivergence::ParameterQualification)
        << "both directions are a conflict — ✔measured, gcc and clang refuse each";
    EXPECT_TRUE(functionRedeclarationCompatibility(in, {fn, &plain}, {fn, &topLvl})
                    .compatible())
        << "6.7.6.3p15 drops a parameter's OWN top-level const";
}

// ★★ ABSENT IS NOT UNQUALIFIED. A side with no claim leaves the axis UNJUDGED —
// it must not be read as "unqualified", which would refuse the ubiquitous legal
// `int printf(const char *, ...);` against a descriptor that spells `ptr<char>`.
TEST(RedeclarationCompat, AnAbsentQualificationClaimIsNotJudged) {
    TypeInterner in = makeInterner();
    TypeId const p  = in.pointer(in.primitive(TypeKind::Char));
    TypeId const fn = in.fnSig(std::vector<TypeId>{p}, in.primitive(TypeKind::I32),
                               CallConv::CcSysV);
    DeclaredQualification pointeeConst;
    pointeeConst.params.push_back(QualifierSpine{0b10, 2});
    EXPECT_TRUE(functionRedeclarationCompatibility(in, {fn, &pointeeConst},
                                                   {fn, nullptr})
                    .compatible());
    // …and a per-PARAMETER absence is just as unjudged as a whole-side one.
    DeclaredQualification noClaimForThatParam;
    noClaimForThatParam.params.push_back(std::nullopt);
    EXPECT_TRUE(functionRedeclarationCompatibility(in, {fn, &pointeeConst},
                                                   {fn, &noClaimForThatParam})
                    .compatible());
}

// ★★ THE PLATFORM MODE. A descriptor states a REPRESENTATION and no source
// spelling, so `long` (a NAMED I64 vocabulary entry) must compare equal to a bare
// `i64` — ✔the exact measurement that made a prior cycle abandon this check.
// Under SourceVocabulary the two stay distinct, because there `long` and
// `long long` are genuinely different declarations.
TEST(RedeclarationCompat, PlatformModeIsSpellingBlindAndSourceModeIsNot) {
    TypeInterner in = makeInterner();
    TypeId const bare  = in.primitive(TypeKind::I64);
    TypeId const named = in.primitive(TypeKind::I64, "long");
    ASSERT_NE(bare.v, named.v) << "vocabulary identity is what this test is about";
    std::vector<TypeId> const none;
    TypeId const fnBare  = in.fnSig(none, bare,  CallConv::CcSysV);
    TypeId const fnNamed = in.fnSig(none, named, CallConv::CcSysV);

    EXPECT_TRUE(compat(in, fnNamed, fnBare, LeafComparison::PlatformVocabulary)
                    .compatible());
    EXPECT_EQ(compat(in, fnNamed, fnBare, LeafComparison::SourceVocabulary).axis,
              RedeclarationDivergence::ReturnType);
    // Spelling-blindness is RECURSIVE — a descriptor's `ptr<i64>` and a source
    // `long *` are one pointer level apart from the leaves that differ.
    TypeId const pBare  = in.pointer(bare);
    TypeId const pNamed = in.pointer(named);
    EXPECT_TRUE(compat(in, in.fnSig(std::vector<TypeId>{pNamed}, bare, CallConv::CcSysV),
                       in.fnSig(std::vector<TypeId>{pBare}, bare, CallConv::CcSysV),
                       LeafComparison::PlatformVocabulary)
                    .compatible());
    // …but a WIDTH difference is never spelled away.
    TypeId const i32 = in.primitive(TypeKind::I32);
    EXPECT_EQ(compat(in, in.fnSig(none, i32, CallConv::CcSysV), fnNamed,
                     LeafComparison::PlatformVocabulary).axis,
              RedeclarationDivergence::ReturnType);
}

// A NOMINAL type is answered by identity alone. `sameRepresentation` would call
// two same-shaped structs equal — right for licensing a retag, and a layout
// miscompile here.
TEST(RedeclarationCompat, TwoDistinctStructsAreNeverSpelledEqual) {
    TypeInterner in = makeInterner();
    TypeId const i32 = in.primitive(TypeKind::I32);
    std::vector<TypeId> const fields{i32};
    TypeId const a = in.structType("A", fields);
    TypeId const b = in.structType("B", fields);
    ASSERT_NE(a.v, b.v);
    std::vector<TypeId> const none;
    EXPECT_EQ(compat(in, in.fnSig(none, in.pointer(a), CallConv::CcSysV),
                     in.fnSig(none, in.pointer(b), CallConv::CcSysV),
                     LeafComparison::PlatformVocabulary).axis,
              RedeclarationDivergence::ReturnType);
}

// ══ 2. USER ↔ USER REDECLARATION, THROUGH THE REAL `c` SCHEMA ════════════════

// ✔BOTH references REFUSE every shape in this list and ACCEPT every shape in the
// next. Before P44 DSS accepted the whole first list.
TEST(RedeclarationCompat, PointeeConstMakesAnIncompatibleRedeclaration) {
    for (char const* src : {
             "extern int f(const char *);\nextern int f(char *);\nint main(void){return 0;}\n",
             "extern int f(char *);\nextern int f(const char *);\nint main(void){return 0;}\n",
             "extern int h(const char **);\nextern int h(char **);\nint main(void){return 0;}\n",
             "extern int uc(const int *const *);\nextern int uc(const int **);\nint main(void){return 0;}\n",
             "extern const char *k(void);\nextern char *k(void);\nint main(void){return 0;}\n",
             "extern int aa(const char x[]);\nextern int aa(char x[]);\nint main(void){return 0;}\n",
             "int z2(const char *s);\nint z2(char *s){return s?1:0;}\nint main(void){return 0;}\n",
             // ★ P44 wave 2 (item 5c): GROUPED declarators. The spine walker used
             // to make NO CLAIM for any parenthesized declarator, because an inner
             // declarator's levels INTERLEAVE with the outer's and a wrong
             // ordering would refuse legal code. The ordering rule is now derived
             // from `declaratorDeclaredType`'s own fold and applied recursively:
             // `ctors(D') ++ suffixes(D) ++ reverse(L(D))`, base underneath. Both
             // shapes are ✔REFUSED by gcc 13.3.0 and clang 18.1.3, separately.
             "extern int pa(const int (*)[3]);\nextern int pa(int (*)[3]);\nint main(void){return 0;}\n",
             "extern int gp(const char *(*));\nextern int gp(char *(*));\nint main(void){return 0;}\n",
             // ★ P44 (item 5b): the `restrict` AXIS. It is a POINTEE qualifier
             // here — the INNER pointer of `char *restrict *` — so 6.7.6.1p2
             // makes it part of the type, exactly as `const` is one level in.
             // ✔REFUSED by gcc 13.3.0 and clang 18.1.3, probed separately; DSS
             // accepted both before the `restrictMarker` role existed, because
             // `restrict` is neither interned nor previously declarable.
             "extern int rp(char *restrict *);\nextern int rp(char **);\nint main(void){return 0;}\n",
             "extern char *restrict *rg(void);\nextern char **rg(void);\nint main(void){return 0;}\n",
             // ★ P44 wave 3 (item 5c): a FUNCTION-POINTER parameter, in both the
             // return and the nested-parameter positions. The return shape rides
             // the FLAT spine — the ordering rule puts the returned pointee under
             // the Fn level, so `const char *(*)(void)` is [Ptr, Fn, Ptr] +
             // base(const) — while the parameter shape needs the nested claim.
             // Both ✔REFUSED by gcc 13.3.0 and clang 18.1.3, probed separately.
             "extern void fr(const char *(*)(void));\nextern void fr(char *(*)(void));\nint main(void){return 0;}\n",
             "extern void fd(int (*)(char *const *));\nextern void fd(int (*)(char **));\nint main(void){return 0;}\n",
         }) {
        auto model = analyzeC(src);
        EXPECT_TRUE(hasCode(model.diagnostics(),
                            DiagnosticCode::S_IncompatibleRedeclaration))
            << "accepted a redeclaration BOTH gcc and clang refuse:\n" << src;
    }
}

TEST(RedeclarationCompat, LegalRedeclarationsStayAccepted) {
    for (char const* src : {
             // 6.7.6.3p15 — a parameter's OWN top-level qualifier is dropped.
             "extern int g(char *const);\nextern int g(char *);\nint main(void){return 0;}\n",
             "int r(const int x);\nint r(int x){return x;}\nint main(void){return r(1);}\n",
             "extern int tq(volatile int);\nextern int tq(int);\nint main(void){return 0;}\n",
             "extern int w2(char *restrict);\nextern int w2(char *);\nint main(void){return 0;}\n",
             // ★★ THE RETURN TYPE'S OWN TOP LEVEL, WHERE THE REFERENCES DISAGREE
             // AND THE BAR IS THE DISJUNCTION. 6.7.6.3p15's unqualifying sentence
             // names PARAMETERS only, and ✔clang 18.1.3 reads it that way and
             // REFUSES both lines below; ✔gcc 13.3.0 ACCEPTS both. One reference
             // accepting a correct construct is the whole test, so DSS accepts.
             // The POINTED-TO twin — refused by gcc AND clang AND mingw — is in
             // the refusal list above, which is what keeps this from being a
             // blanket relaxation of the return axis.
             "const int cf(void);\nint cf(void);\nint main(void){return 0;}\n",
             "char *const cg(void);\nchar *cg(void);\nint main(void){return 0;}\n",
             // identical, and proto-then-definition
             "extern int y(const char *);\nextern int y(const char *);\nint main(void){return 0;}\n",
             "int z(const char *s);\nint z(const char *s){return s?1:0;}\nint main(void){return z(\"a\");}\n",
             // The grouped ACCEPTING twin — identical grouped declarators must
             // stay compatible. Without it, "the group arm works" is
             // indistinguishable from "the group arm refuses everything".
             "extern int ok(const int (*)[3]);\nextern int ok(const int (*)[3]);\nint main(void){return 0;}\n",
             // ★ P44 (item 5b): the ACCEPTING twins of the two `restrict`
             // refusals above. Without these, "the restrict axis works" is
             // indistinguishable from "the restrict axis refuses everything" —
             // and the second shape is the one that proves the level-0 mask
             // applies to `restrict` and not only to `const`.
             // ✔ACCEPTED by gcc 13.3.0 and clang 18.1.3, probed separately.
             "extern int rp(char *restrict *);\nextern int rp(char *restrict *);\nint main(void){return 0;}\n",
             "extern char *restrict *rg(void);\nextern char *restrict *rg(void);\nint main(void){return 0;}\n",
         }) {
        auto model = analyzeC(src);
        EXPECT_FALSE(model.hasErrors())
            << "refused a redeclaration BOTH gcc and clang accept:\n"
            << src << "  first error: " << firstMessage(model);
    }
}

// ★★ P44 — THE `restrict` AXIS, AND THE THREE-WAY DISCRIMINATION THAT MAKES IT A
// CLAIM RATHER THAN A SWITCH (D-C23-REDECL-QUALIFIER-AXIS-HAS-THREE-UNCLAIMED-SOURCES,
// part (b)).
//
// `restrict` is neither interned (it is an ALIASING promise — no layout, no
// calling convention, no codegen DSS performs) nor, before this cycle, declarable
// in any shipped language config, so the spine could make no claim about it and
// the oracle could not judge the axis at all. It now rides `QualifierSpine`
// beside `const`, driven by a per-declaration `restrictMarker` role.
//
// THE THREE CASES ARE ASSERTED TOGETHER because any two of them alone would pass
// for a mistake:
//   * a POINTEE `restrict` DIVERGING  → refused (both references refuse);
//   * a POINTEE `restrict` MATCHING   → accepted (both accept);
//   * a TOP-LEVEL parameter `restrict` diverging → accepted, because 6.7.6.3p15
//     drops a parameter's own top-level qualifier and says nothing about WHICH
//     qualifier (both references accept).
// A walk that set the bit from the HEAD instead of from the pointer layer would
// pass the first and fail the third; one that never set the bit would pass the
// last two.
//
// RED-ON-DISABLE (REMOVE direction): delete `"restrictMarker": "RestrictKeyword"`
// from `param`'s declaration row in `c.lang.json` and the first case goes green
// into silence — the refusal disappears while every other assertion here still
// passes, which is exactly why it is asserted by NAME and not by a count.
TEST(RedeclarationCompat, RestrictIsJudgedOnPointeesAndDroppedAtAParameterTopLevel) {
    auto const refused = [](char const* src) {
        auto model = analyzeC(src);
        return hasCode(model.diagnostics(),
                       DiagnosticCode::S_IncompatibleRedeclaration);
    };
    EXPECT_TRUE(refused(
        "extern int rp(char *restrict *);\nextern int rp(char **);\n"
        "int main(void){return 0;}\n"))
        << "a POINTEE restrict is part of the type (6.7.6.1p2) — gcc 13.3.0 and "
           "clang 18.1.3 both refuse this pair";
    EXPECT_FALSE(refused(
        "extern int rp(char *restrict *);\nextern int rp(char *restrict *);\n"
        "int main(void){return 0;}\n"))
        << "…and two identical spines must stay compatible, or the axis is "
           "refusing everything rather than judging anything";
    EXPECT_FALSE(refused(
        "extern int w2(char *restrict);\nextern int w2(char *);\n"
        "int main(void){return 0;}\n"))
        << "a parameter's OWN top-level restrict is dropped by 6.7.6.3p15, which "
           "names no particular qualifier — both references accept this";
}

// The structural axes keep working through the same oracle — these were already
// refused before P44 and must not have been relaxed by the qualifier work.
TEST(RedeclarationCompat, StructuralIncompatibilityStaysRefused) {
    for (char const* src : {
             "extern int p();\nextern int p(int, int);\nint main(void){return 0;}\n",
             "extern int q(const char *, ...);\nextern int q();\nint main(void){return 0;}\n",
             "extern int v(volatile char *);\nextern int v(char *);\nint main(void){return 0;}\n",
             "extern int ta(_Atomic int);\nextern int ta(int);\nint main(void){return 0;}\n",
             "extern int n(int);\nextern long n(int);\nint main(void){return 0;}\n",
         }) {
        auto model = analyzeC(src);
        EXPECT_TRUE(hasCode(model.diagnostics(),
                            DiagnosticCode::S_IncompatibleRedeclaration))
            << src;
    }
}

// ══ 3. THE SHIPPED-DESCRIPTOR TIER ═══════════════════════════════════════════

// ★★ THE ROW'S OWN REPRODUCER. Before P44 this compiled with ZERO diagnostics on
// both legs and called the platform's real `puts` with a double in xmm0 where it
// expects a `char *` in rcx. ✔gcc, clang and mingw gcc all refuse it.
TEST(RedeclarationCompat, ShippedRowSignatureIsCheckedAgainstTheUserDeclaration) {
    for (char const* src : {
             "#include <stdio.h>\nint puts(double x);\nint main(void){puts(3.5);return 0;}\n",
             "#include <stdio.h>\nint puts(const char *s, int n);\nint main(void){return 0;}\n",
             "#include <stdio.h>\nlong puts(const char *s);\nint main(void){return 0;}\n",
             "#include <stdio.h>\nextern int fprintf(void *, const char *, ...);\nint main(void){return 0;}\n",
         }) {
        auto model = analyzeElf(src);
        EXPECT_TRUE(hasCode(model.diagnostics(),
                            DiagnosticCode::S_IncompatibleRedeclaration))
            << "a shipped row was suppressed on its NAME alone:\n" << src;
    }
}

// The accepting twin — the declaration a real program writes. A check that
// refuses these is worse than no check at all.
TEST(RedeclarationCompat, MatchingUserDeclarationOfAShippedNameStaysAccepted) {
    for (char const* src : {
             "#include <stdio.h>\nint puts(const char *s);\nint main(void){puts(\"hi\");return 0;}\n",
             "#include <stdio.h>\nextern int printf(const char *, ...);\nint main(void){printf(\"hi\");return 0;}\n",
             // ★★ P44 wave 4 — THIS ENTRY MOVED TO THE REFUSAL SIDE, AND ITS
             // OWN COMMENT NAMED THE CONDITION. It read: "6.7.6.1p2 CANNOT be
             // enforced against a descriptor (hir-text has no `const` spelling),
             // so a pointee-const difference is UNJUDGED here". hir-text now HAS
             // that spelling, `stdio.json`'s `printf` carries
             // `ptr<const<char>>`, and `extern int printf(char *, ...);` is
             // refused — which gcc 13.3.0 and clang 18.1.3 both do (probed
             // separately). It is asserted in
             // `ShippedRowQualifierIsCheckedAgainstTheUserDeclaration` below.
             // ⛔ The warning it carried STILL BINDS and is now enforced by
             // `AnUnannotatedShippedRowMakesNoQualifierClaim`: closing this by
             // reading an ABSENT claim as "unqualified" would red the LINE ABOVE,
             // which is what real code writes, and it did exactly that on DSS's
             // own runtime shims before the producer gate landed.
             // 6.7.6.3p7: an array parameter adjusts to a pointer, so this IS the
             // descriptor's `ptr<char>`.
             "#include <stdio.h>\nint puts(const char s[]);\nint main(void){return 0;}\n",
         }) {
        auto model = analyzeElf(src);
        EXPECT_FALSE(model.hasErrors())
            << "refused a legal declaration of a shipped name:\n"
            << src << "  first error: " << firstMessage(model);
    }
}

// ★★ THE TIER, WHICH IS THE WHOLE OF THE THIRD ROW. `extern int printf();`
// applies C23's `()` ≡ `(void)` correctly — that half is right and is untouched —
// but the only complaint used to be an ARITY error at the CALL, so the SAME
// program with NO CALL compiled clean. The no-call program is the sharpest
// available witness that the diagnostic moved tiers rather than changing wording.
TEST(RedeclarationCompat, IncompatibleRedeclarationIsDiagnosedAtTheDeclaration) {
    constexpr char const* kNoCall =
        "#include <stdio.h>\nextern int printf();\nint main(void){return 0;}\n";
    constexpr char const* kWithCall =
        "#include <stdio.h>\nextern int printf();\nint main(void){printf(\"hi\");return 0;}\n";

    for (auto* analyzeLeg : {&analyzeElf, &analyzePe}) {
        auto noCall = (*analyzeLeg)(kNoCall);
        EXPECT_TRUE(hasCode(noCall.diagnostics(),
                            DiagnosticCode::S_IncompatibleRedeclaration))
            << "a program with the bad declaration and NO CALL compiled clean — "
               "the defect hides in exactly the case nothing else catches";
        EXPECT_FALSE(hasCode(noCall.diagnostics(),
                             DiagnosticCode::S_ArgCountMismatch))
            << "there is no call to blame";

        // ⚠ THE FAILURE ARM: the call-site arity check must NOT have been
        // suppressed to make room for the declaration-site one. With a call
        // present BOTH fire — the declaration error is the new one, and the arity
        // error is the old behaviour, unchanged.
        auto withCall = (*analyzeLeg)(kWithCall);
        EXPECT_TRUE(hasCode(withCall.diagnostics(),
                            DiagnosticCode::S_IncompatibleRedeclaration));
        EXPECT_TRUE(hasCode(withCall.diagnostics(),
                            DiagnosticCode::S_ArgCountMismatch))
            << "the call-site arity check was suppressed rather than joined — a "
               "message change wearing a tier change's clothes";
    }
}

// ★★★ THE SCOPE GUARD, AND IT IS THE MOST LOAD-BEARING TEST IN THIS FILE.
// C23 6.7p4 makes a declaration conflict with the implementation's only when the
// implementation's is IN SCOPE — i.e. the header was actually included. A program
// that declares (or DEFINES) a name itself and includes nothing has nothing to
// conflict with, and ✔gcc, clang and mingw gcc all accept every line below.
//
// ✔MEASURED: with the check run over every corpus-realized name instead of only
// the `#include`-suppressed ones, ALL FIVE of these were refused. The `static`
// one is the sharpest — an internal-linkage function cannot be the platform's
// symbol under any reading.
TEST(RedeclarationCompat, ANameTheSourceNeverIncludedIsNotJudgedAgainstTheCorpus) {
    for (char const* src : {
             "extern int read(char *b);\nint main(void){(void)read;return 0;}\n",
             "int strlen(const char *s);\nint main(void){return strlen(\"a\");}\n",
             "int puts(double x){(void)x;return 0;}\nint main(void){return puts(1.0);}\n",
             "static int puts(double x){(void)x;return 0;}\nint main(void){return puts(1.0);}\n",
             "extern int fprintf(void *, const char *, ...);\nint main(void){return 0;}\n",
         }) {
        for (auto* analyzeLeg : {&analyzeElf, &analyzePe}) {
            auto model = (*analyzeLeg)(src);
            EXPECT_FALSE(hasCode(model.diagnostics(),
                                 DiagnosticCode::S_IncompatibleRedeclaration))
                << "judged a declaration against a descriptor the source never "
                   "included — every reference accepts this:\n"
                << src;
        }
    }
}

// ★★ P44 wave 4 (item (a)) — THE PLATFORM SIDE CAN NOW SPELL A QUALIFIER, AND
// THIS IS THE PAIR THAT PROVES BOTH HALVES OF IT.
//
// A descriptor signature is hir-text, which had no `const` spelling at all, so
// the corpus made NO qualification claim and C23 6.7.6.1p2 could not be enforced
// against it: ✔MEASURED before this, `extern int printf(char *, ...);` over
// `#include <stdio.h>` compiled CLEAN while gcc 13.3.0 (`-std=c2x`) and clang
// 18.1.3 (`-std=c23`), probed SEPARATELY, both REFUSE it. The grammar now has
// `const<…>` / `restrict<…>`, `printf` ships as `fn(ptr<const<char>>, ...) ->
// i32`, and the claim rides beside the TypeId — which it must, because neither
// qualifier is interned and the two spellings are ONE TypeId by design.
//
// RED-ON-DISABLE: unwrap `printf`'s `ptr<const<char>>` back to `ptr<char>` in
// `stdio.json` and the first case goes green into silence.
TEST(RedeclarationCompat, ShippedRowQualifierIsCheckedAgainstTheUserDeclaration) {
    auto bad = analyzeElf(
        "#include <stdio.h>\nextern int printf(char *, ...);\n"
        "int main(void){return 0;}\n");
    EXPECT_TRUE(hasCode(bad.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration))
        << "gcc and clang both refuse this; DSS accepted it before the corpus "
           "could spell a qualifier";
    auto good = analyzeElf(
        "#include <stdio.h>\nextern int printf(const char *, ...);\n"
        "int main(void){return 0;}\n");
    EXPECT_FALSE(hasCode(good.diagnostics(),
                         DiagnosticCode::S_IncompatibleRedeclaration))
        << "…and the twin BOTH references accept must stay accepted, or the "
           "claim is refusing everything rather than judging anything";
}

// ★★★ THE OVER-REACH DETECTOR, AND IT IS THE HALF THAT GOVERNS THE ~1500 CORPUS
// ROWS NOBODY HAS ANNOTATED. An UNANNOTATED row spells no qualifier and must
// therefore make NO CLAIM — not a claim of "unqualified". Reading silence as a
// statement is not a hypothetical failure: ✔MEASURED while building this, with
// the producer's gate missing, DSS's OWN shipped runtime shims stopped
// compiling — `void *opendir(const char *name)` was refused against
// `dirent.json`'s unannotated `fn(ptr<char>) -> ptr<void>`, and `int
// truncate(const char *, long)` likewise. Both are correct C that gcc and clang
// accept.
// RED-ON-DISABLE: drop `qualSawQualifier_` from `parseTypeFromTextEntry`'s gate
// in `hir_text.cpp` and every case here reds.
TEST(RedeclarationCompat, AnUnannotatedShippedRowMakesNoQualifierClaim) {
    for (char const* src : {
             // `fputs` is unannotated; a `const char *` declaration of it is
             // legal C and must not meet a fabricated "the row says char *".
             "#include <stdio.h>\nextern int fputs(const char *, FILE *);\n"
             "int main(void){return 0;}\n",
             // …and the reverse direction on the same row.
             "#include <stdio.h>\nextern int fputs(char *, FILE *);\n"
             "int main(void){return 0;}\n",
         }) {
        auto model = analyzeElf(src);
        EXPECT_FALSE(hasCode(model.diagnostics(),
                             DiagnosticCode::S_IncompatibleRedeclaration))
            << "an unannotated row states nothing about the qualifier axis:\n"
            << src;
    }
}

// A shipped DATA row is not a function and is not judged by this oracle: a
// hand-declared `extern FILE *stdout;` and the descriptor's object row diverge
// legitimately, and over-refusing one would break every program that writes it.
TEST(RedeclarationCompat, ShippedObjectRowsAreNotJudged) {
    auto model = analyzeElf(
        "#include <stdio.h>\nextern struct _IO_FILE *stdout;\n"
        "int main(void){return 0;}\n");
    EXPECT_FALSE(hasCode(model.diagnostics(),
                         DiagnosticCode::S_IncompatibleRedeclaration));
}

// ★★ P44 wave 3 — THE HOLE IS FILLED, AND THIS PIN FLIPPED EXACTLY AS ITS OWN
// FAILURE MESSAGE INSTRUCTED. It used to assert the ACCEPTANCE of
// `int (*)(const char *)` beside `int (*)(char *)` and to say, in its message,
// that a red here means the recursive claim has landed. It went red; this is the
// flip. (Item (c) of D-C23-REDECL-QUALIFIER-AXIS-HAS-THREE-UNCLAIMED-SOURCES.)
//
// WHAT MADE IT DERIVABLE RATHER THAN A GUESS. The prior wave stopped because a
// fn suffix means two things and "the level math stops being derivable". It does
// not: the ordering rule `ctors(D') ++ suffixes(D) ++ reverse(L(D))`, base
// underneath, already places the head qualifier correctly for a function pointer
// — ✔CHECKED against `declaratorDeclaredType`'s own fold, `const char *(*)(void)`
// folds to [Ptr, Fn, Ptr] + base(const), and that base IS the returned pointee,
// which is where 6.7.6.1p2 wants the claim. So the RETURN type never needed a
// special case; it is the deeper levels of the same spine. Only the inner
// function's PARAMETERS are unreachable from a flat chain, and those now ride a
// nested `DeclaredQualification` keyed by level.
//
// ✔MEASURED THROUGH THE SHIPPED CLI, all eight shapes, gcc 13.3.0 (`-std=c2x`)
// and clang 18.1.3 (`-std=c23`) probed SEPARATELY — DSS now agrees with both on
// every one: REFUSED `fp(int (*)(const char *))` vs `(char *)`, `fr(const char
// *(*)(void))` vs `(char *(*)(void))`, `fd(int (*)(char *const *))` vs
// `(char **)`; ACCEPTED each of those against its identical twin, plus
// `ft(int (*)(char *const))` vs `(char *)` (the inner parameter's OWN top level,
// dropped by 6.7.6.3p15) and `fv(int (*)(void))` vs `(int (*)())` (two spellings
// of one type, where the nested claim must stay SILENT rather than key on a row
// count).
TEST(RedeclarationCompat, AFunctionPointerParameterQualifierIsClaimed) {
    auto model = analyzeC(
        "extern int fp(int (*)(const char *));\n"
        "extern int fp(int (*)(char *));\n"
        "int main(void){return 0;}\n");
    EXPECT_TRUE(hasCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration))
        << "gcc 13.3.0 and clang 18.1.3 both refuse this pair";
}

// ★★★ THE CONTROLS, AND THEY ARE WHAT SEPARATE "THE RECURSION WORKS" FROM "THE
// RECURSION REFUSES EVERY FUNCTION POINTER". The refusal above is worth nothing
// without them: a nested claim that fabricated a divergence would pass it and
// fail all four of these, and refusing legal code is the one outcome this oracle
// may never produce.
//
// The last two are the sharpest. `ft` puts the qualifier on the INNER function's
// own top-level parameter, which 6.7.6.3p15 drops — a recursion that forgot to
// mask level 0 inside would refuse it. `fv` spells one type two ways, giving the
// two nested claims a DIFFERENT NUMBER of parameter rows — a recursion that
// treated a count mismatch as a divergence would refuse it.
// ✔ACCEPTED by gcc 13.3.0 and clang 18.1.3, probed separately.
TEST(RedeclarationCompat, FunctionPointerQualifierAcceptsWhatBothReferencesAccept) {
    for (char const* src : {
             "extern int fp(int (*)(const char *));\n"
             "extern int fp(int (*)(const char *));\n"
             "int main(void){return 0;}\n",
             "extern int fq(int (*)(char *));\n"
             "extern int fq(int (*)(char *));\n"
             "int main(void){return 0;}\n",
             "extern int ft(int (*)(char *const));\n"
             "extern int ft(int (*)(char *));\n"
             "int main(void){return 0;}\n",
             "extern int fv(int (*)(void));\n"
             "extern int fv(int (*)());\n"
             "int main(void){return 0;}\n",
         }) {
        auto model = analyzeC(src);
        EXPECT_FALSE(model.hasErrors())
            << "refused a function-pointer redeclaration BOTH gcc and clang "
               "accept:\n" << src << "  first error: " << firstMessage(model);
    }
}
