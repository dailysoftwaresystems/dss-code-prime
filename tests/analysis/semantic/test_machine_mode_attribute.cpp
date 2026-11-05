// ═══════════════════════════════════════════════════════════════════════════
//  GNU `mode(M)` — THE DECLARED TYPE REPLACED BY A MACHINE MODE'S
//  cycle P69 round 1, fold f26c, lane `cs`
//
//  Until this fold the attribute was refused by name. Before that refusal it
//  was warned as unknown, and `int x __attribute__((mode(DI)));` was a 4-byte
//  object where every reference makes it 8. The modes are now a table of the C
//  language document (`effects[].machineModes`), and the analyzer has ONE
//  substitution that reads it (`machineModeType`).
// ═══════════════════════════════════════════════════════════════════════════
//
// ── THE REFERENCE MATRIX (lane `cs`'s probes md and md2, each cell compiled and
//    RUN through `dssharness run probe-reference-cc`, 2026-11-03) ─────────────
//  gcc 13.3.0 and clang 18.1.3 (Linux x86_64) · mingw-w64 gcc 13.2.0 · Apple
//  clang (macOS arm64) · aarch64-linux-gnu gcc 13.3.0. cl has no `__attribute__`.
//
//  INTEGER MODES on `int` / `unsigned`, typedef or declaration, plain or `__X__`:
//      QI 1  HI 2  SI 4  DI 8  TI 16  byte 1  word 8  pointer 8  unwind_word 8
//  with the declared type's signedness (`(T)-1 < 0`), on every reference. The
//  TYPE is the first standard type of the width: `_Generic` picks `long` for DI
//  on LP64 (`long long` on mingw's LLP64), `signed char` for QI, `unsigned
//  char` for an unsigned QI.
//  POSITIONS: a trailing mode is its own declarator's (`int g3 mode(QI), g4;`
//  is 1 and 4), a leading one, a member's, a parameter's and a local's all
//  apply.
//  FLOATING MODES: SF float, DF double. XF and TF: gcc, clang and mingw on
//  x86_64 accept both; Apple clang (arm64) refuses both; aarch64 gcc refuses XF
//  and accepts TF.
//  ENUMERATIONS: `enum E { EA = 1, EB = 2 } __attribute__((mode(QI)))` is 1
//  byte; a typedef of an enumeration with DI is 8; an object of an enumeration
//  type with HI is 2. A value past the width (`GB = 300` with QI): gcc refuses,
//  clang accepts.
//  `_Bool` with DI: gcc refuses; clang 8 bytes, unsigned. Two modes on one
//  declaration: the LAST stands. `const int` with DI: 8.
//  A POINTER with DI: gcc accepts (8), clang refuses; with SI every reference
//  refuses. REFUSED BY EVERY REFERENCE: an unknown name, `mode` with no operand,
//  SF on `int`, DI on `float`, an array, a function. A vector mode (V4SI) is
//  accepted by every reference with a deprecation warning.
//
//  THE DECISIONS (findings fe178; the row says why): a type of a representation
//  the target's C does not have is refused by name (TF on x86_64, XF on pe64 and
//  Apple arm64 — gcc and clang mint one there); a vector mode is refused (this
//  compiler has no vector types); `_Bool` takes clang's answer; an enumerator past
//  the width is refused (gcc's); a pointer keeps its type at its own width (gcc's).
//
// Every pin asserts a NUMBER a reference printed or a refusal every reference
// gives, never merely "no error": the defect was a clean compile at the wrong size.

#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include "semantic_test_fixture.hpp"
#include "test_support/repo_root.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

// Layout parameters are REQUIRED: the pointer-width modes read the pointer's
// layout, and every size below is a layout.
constexpr AggregateLayoutParams kLayout{
    .scalarAlignment       = ScalarAlignmentRule::Natural,
    .maxAlignment          = 16,
    .maxRequestedAlignment = 268435456};

[[nodiscard]] SemanticModel analyzeC(std::string const& src,
                                     DataModel dm = DataModel::Lp64,
                                     LongDoubleFormat ldf = LongDoubleFormat::X87_80) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), dm, kLayout, std::nullopt,
                   std::nullopt, std::nullopt, ldf);
}

[[nodiscard]] SymbolRecord const* findSym(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return &m.symbols()[i];
    return nullptr;
}

[[nodiscard]] TypeId typeOf(SemanticModel const& m, std::string_view name) {
    SymbolRecord const* s = findSym(m, name);
    EXPECT_NE(s, nullptr) << "no symbol named '" << name << "'";
    return s == nullptr ? InvalidType : s->type;
}

[[nodiscard]] std::uint64_t sizeOfType(SemanticModel const& m, TypeId t) {
    auto const l = computeLayout(t, m.lattice().interner(), kLayout, DataModel::Lp64);
    EXPECT_TRUE(l.has_value()) << "the type has no layout";
    return l ? static_cast<std::uint64_t>(l->size) : 0;
}

[[nodiscard]] std::uint64_t sizeOf(SemanticModel const& m, std::string_view name) {
    TypeId const t = typeOf(m, name);
    return t.valid() ? sizeOfType(m, t) : 0;
}

[[nodiscard]] std::string everyDiagnostic(SemanticModel const& m) {
    std::string out;
    for (auto const& d : m.diagnostics().all()) {
        out += "\n  ";
        out += diagnosticCodeName(d.code);
        out += ": ";
        out += d.actual;
    }
    return out.empty() ? std::string{" <none>"} : out;
}

[[nodiscard]] std::string replaceAll(std::string text, std::string_view from,
                                     std::string_view to) {
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

// The integer kind a mode of `bytes` makes of a type of this signedness.
[[nodiscard]] TypeKind integerKind(std::uint64_t bytes, bool isSigned) {
    switch (bytes) {
        case 1:  return isSigned ? TypeKind::I8 : TypeKind::U8;
        case 2:  return isSigned ? TypeKind::I16 : TypeKind::U16;
        case 4:  return isSigned ? TypeKind::I32 : TypeKind::U32;
        case 8:  return isSigned ? TypeKind::I64 : TypeKind::U64;
        default: return isSigned ? TypeKind::I128 : TypeKind::U128;
    }
}

// The shipped C document with ONE piece of its text replaced, loaded as a
// language of its own; returns every load error's message joined, or empty when
// the edited document LOADS. `expected` is how many occurrences the edit must
// find — a count that moved means the document changed under the pin.
[[nodiscard]] std::string loadErrorsOfEditedC(std::string_view from, std::string_view to,
                                              std::size_t expected) {
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    std::ifstream in(*root / "sources" / "c.lang.json", std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot read the shipped c document";
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    std::size_t edits = 0;
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
        ++edits;
    }
    EXPECT_EQ(edits, expected) << "occurrences of the text this pin edits: " << from;
    auto loaded = GrammarSchema::loadFromText(text, "<shipped c, edited>");
    if (loaded.has_value()) return {};
    std::string all;
    for (auto const& d : loaded.error()) {
        all += d.message;
        all += '\n';
    }
    return all.empty() ? std::string{"<a refusal with no message>"} : all;
}

} // namespace

// ── THE INTEGER MODES ──────────────────────────────────────────────────────

// Every integer mode name × both signednesses × both spellings × every position
// a declaration can put it in: a typedef, a file-scope object, a member, a
// parameter (its own type AND the signature's), a local. One source per cell, so
// a red names the mode, the spelling and the sign.
//
// RED-ON-DISABLE: make `machineModeType` return the declared type and every cell
// keeps 4 bytes; drop the `machineMode` clause from the fold
// (`scanAttributeSemantics`' `MachineMode` arm) and the same; skip the harvest's
// substitution (`declRowDeclaredType`) and only the signature's cells go red.
TEST(MachineModeAttribute, EveryIntegerModeIsItsWidthWithTheDeclaredSignednessAtEveryPosition) {
    struct Mode { char const* name; std::uint64_t bytes; };
    constexpr Mode kModes[] = {{"QI", 1},   {"HI", 2},      {"SI", 4},
                               {"DI", 8},   {"TI", 16},     {"byte", 1},
                               {"word", 8}, {"pointer", 8}, {"unwind_word", 8}};
    constexpr char const* kTemplate =
        "typedef T t_ __attribute__((mode(M)));\n"
        "T o_ __attribute__((mode(M)));\n"
        "__attribute__((mode(M))) T lead_;\n"
        "struct S_ { char c; T m_ __attribute__((mode(M))); };\n"
        "struct S_ s_;\n"
        "int f_(T p_ __attribute__((mode(M)))) {\n"
        "    T l_ __attribute__((mode(M))) = 0;\n"
        "    return (int)sizeof p_ + (int)sizeof l_;\n"
        "}\n";
    for (Mode const& mode : kModes) {
        for (bool const dunder : {false, true}) {
            for (bool const isSigned : {true, false}) {
                std::string const spelled =
                    dunder ? "__" + std::string{mode.name} + "__" : std::string{mode.name};
                std::string src = replaceAll(kTemplate, "mode(M)", "mode(" + spelled + ")");
                src = replaceAll(src, "T ", isSigned ? "int " : "unsigned ");
                auto const m = analyzeC(src);
                SCOPED_TRACE(src);
                EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
                EXPECT_TRUE(m.diagnostics().all().empty()) << everyDiagnostic(m);
                TypeInterner const& in = m.lattice().interner();
                TypeKind const want = integerKind(mode.bytes, isSigned);
                for (char const* name : {"t_", "o_", "lead_", "p_", "l_"}) {
                    EXPECT_EQ(sizeOf(m, name), mode.bytes) << name;
                    TypeId const t = typeOf(m, name);
                    if (t.valid()) EXPECT_EQ(in.kind(t), want) << name;
                }
                // The member: a char, then the member aligned to its own width.
                EXPECT_EQ(sizeOf(m, "s_"), 2 * mode.bytes) << "the member";
                // The signature the calls are checked and lowered against.
                TypeId const fn = typeOf(m, "f_");
                ASSERT_TRUE(fn.valid());
                ASSERT_EQ(in.kind(fn), TypeKind::FnSig);
                ASSERT_EQ(in.fnParams(fn).size(), 1u);
                EXPECT_EQ(sizeOfType(m, in.fnParams(fn)[0]), mode.bytes) << "the signature";
                EXPECT_EQ(in.kind(in.fnParams(fn)[0]), want) << "the signature";
            }
        }
    }
}

// A mode's type IS a standard type — the FIRST of its width in the language's
// own list — so it is compatible with, and selects in `_Generic` as, that type.
// DI is `long` on LP64 and `long long` on LLP64.
//
// RED-ON-DISABLE: intern the anonymous representative (`primitive(core)`) instead
// of the row's (`primitive(core, ts.name)`) and DI stops being `long`.
TEST(MachineModeAttribute, ItsTypeIsTheFirstStandardTypeOfItsWidth) {
    {
        auto const m = analyzeC(
            "long rl; unsigned long rul; int ri; signed char rsc; unsigned char ruc;\n"
            "short rs; long long rll;\n"
            "int di __attribute__((mode(DI)));\n"
            "unsigned udi __attribute__((mode(DI)));\n"
            "int si __attribute__((mode(SI)));\n"
            "int qi __attribute__((mode(QI)));\n"
            "unsigned uqi __attribute__((mode(QI)));\n"
            "int hi __attribute__((mode(HI)));\n");
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(typeOf(m, "di").v, typeOf(m, "rl").v) << "DI is `long` on LP64";
        EXPECT_NE(typeOf(m, "di").v, typeOf(m, "rll").v) << "and not `long long`";
        EXPECT_EQ(typeOf(m, "udi").v, typeOf(m, "rul").v);
        EXPECT_EQ(typeOf(m, "si").v, typeOf(m, "ri").v);
        EXPECT_EQ(typeOf(m, "qi").v, typeOf(m, "rsc").v);
        EXPECT_EQ(typeOf(m, "uqi").v, typeOf(m, "ruc").v);
        EXPECT_EQ(typeOf(m, "hi").v, typeOf(m, "rs").v);
    }
    {
        auto const m = analyzeC("long long rll; unsigned long long rull; long rl;\n"
                                "int di __attribute__((mode(DI)));\n"
                                "unsigned udi __attribute__((mode(DI)));\n",
                                DataModel::Llp64);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(typeOf(m, "di").v, typeOf(m, "rll").v) << "DI is `long long` on LLP64";
        EXPECT_NE(typeOf(m, "di").v, typeOf(m, "rl").v) << "`long` is 4 bytes there";
        EXPECT_EQ(typeOf(m, "udi").v, typeOf(m, "rull").v);
    }
}

// Plain `char` keeps the TARGET's signedness (signed where no target says
// otherwise), and `_Bool` becomes an UNSIGNED integer of the width (clang's
// answer; gcc refuses it).
TEST(MachineModeAttribute, PlainCharKeepsTheTargetsSignednessAndBoolBecomesUnsigned) {
    auto const m = analyzeC("typedef char C16 __attribute__((mode(HI)));\n"
                            "typedef _Bool B8 __attribute__((mode(DI)));\n");
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    TypeInterner const& in = m.lattice().interner();
    EXPECT_EQ(in.kind(typeOf(m, "C16")), TypeKind::I16);
    EXPECT_EQ(in.kind(typeOf(m, "B8")), TypeKind::U64);
}

// Of two modes on one declaration the LAST stands — in one specifier and across
// two. A declaration-level mode reaches every declarator; a trailing one only its
// own. A qualifier stays on the result: `volatile` is the one the type lattice
// carries (`const` rides the symbol, not the type), and the replacement keeps it —
// DSS's rule that a substitution drops nothing the declaration wrote; the
// references were not probed for it.
//
// RED-ON-DISABLE: keep the FIRST clause in the fold (`if (out.machineModeRow ==
// nullptr)`) and `Two` is 1, `TwoRev` 8.
TEST(MachineModeAttribute, TheLastModeStandsAndEachDeclaratorKeepsItsOwn) {
    auto const m = analyzeC(
        "typedef int Two __attribute__((mode(QI), mode(DI)));\n"
        "typedef int TwoRev __attribute__((mode(DI))) __attribute__((mode(QI)));\n"
        "typedef const int CDI __attribute__((mode(DI)));\n"
        "typedef volatile int VDI __attribute__((mode(DI)));\n"
        "int g3 __attribute__((mode(QI))), g4;\n"
        "__attribute__((mode(DI))) int a, b;\n");
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    EXPECT_EQ(sizeOf(m, "Two"), 8u);
    EXPECT_EQ(sizeOf(m, "TwoRev"), 1u);
    EXPECT_EQ(sizeOf(m, "CDI"), 8u);
    EXPECT_EQ(sizeOf(m, "VDI"), 8u);
    EXPECT_TRUE(m.lattice().interner().isVolatileQualified(typeOf(m, "VDI")))
        << "the `volatile` stays on the moded type";
    EXPECT_EQ(sizeOf(m, "g3"), 1u);
    EXPECT_EQ(sizeOf(m, "g4"), 4u) << "a trailing mode is its own declarator's";
    EXPECT_EQ(sizeOf(m, "a"), 8u);
    EXPECT_EQ(sizeOf(m, "b"), 8u) << "a leading mode is every declarator's";
}

// A parameter declared by its type alone takes the mode its trailing run writes,
// in the signature, and nothing is said about it. ✔MEASURED (lane `cs`'s probe
// np): gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0 and Apple clang 21.0.0 take
// `int f(int __attribute__((mode(QI))));` with a `signed char` definition and
// refuse it with an `int` one ("conflicting types").
TEST(MachineModeAttribute, AParameterWithNoNameTakesItsModeInTheSignature) {
    auto const m = analyzeC("void f(int __attribute__((mode(DI))));\n");
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    EXPECT_TRUE(m.diagnostics().all().empty()) << everyDiagnostic(m);
    TypeInterner const& in = m.lattice().interner();
    TypeId const fn = typeOf(m, "f");
    ASSERT_TRUE(fn.valid());
    ASSERT_EQ(in.kind(fn), TypeKind::FnSig);
    ASSERT_EQ(in.fnParams(fn).size(), 1u);
    ASSERT_TRUE(in.fnParams(fn)[0].valid())
        << "the parameter's type was lost: its trailing attribute run was read as "
           "its declarator";
    EXPECT_EQ(sizeOfType(m, in.fnParams(fn)[0]), 8u);

    auto const agrees = analyzeC("int g(int __attribute__((mode(QI))));\n"
                                 "int g(signed char c) { return c; }\n");
    EXPECT_FALSE(agrees.hasErrors()) << everyDiagnostic(agrees);
    auto const conflicts = analyzeC("int h(int __attribute__((mode(QI))));\n"
                                    "int h(int c) { return c; }\n");
    EXPECT_EQ(countCode(conflicts.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 1u)
        << everyDiagnostic(conflicts);
}

// ── THE FLOATING MODES ─────────────────────────────────────────────────────

// SF and DF are float and double everywhere. XF and TF are the type of that
// representation the target's C has — `long double` where it is that — and a
// target with none refuses the name.
TEST(MachineModeAttribute, FloatingModesAreTheTargetsTypeOfThatRepresentation) {
    constexpr char const* kBase =
        "float rf; double rd; long double rld;\n"
        "float sf __attribute__((mode(SF)));\n"
        "double df __attribute__((mode(DF)));\n"
        "long double ldf __attribute__((mode(DF)));\n";
    {   // x87 `long double` (SysV x86_64): XF is it, TF is refused.
        auto const m = analyzeC(std::string{kBase}
                                + "long double xf __attribute__((mode(XF)));\n");
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(typeOf(m, "sf").v, typeOf(m, "rf").v);
        EXPECT_EQ(typeOf(m, "df").v, typeOf(m, "rd").v);
        EXPECT_EQ(typeOf(m, "ldf").v, typeOf(m, "rd").v) << "DF on `long double` is `double`";
        EXPECT_EQ(typeOf(m, "xf").v, typeOf(m, "rld").v);
        auto const tf = analyzeC("long double tf __attribute__((mode(TF)));\n");
        EXPECT_EQ(countCode(tf.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 1u)
            << everyDiagnostic(tf);
    }
    {   // binary128 `long double` (AAPCS64 Linux): TF is it, XF is refused.
        auto const m = analyzeC("long double rld;\n"
                                "long double tf __attribute__((mode(TF)));\n",
                                DataModel::Lp64, LongDoubleFormat::Ieee128);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(typeOf(m, "tf").v, typeOf(m, "rld").v);
        auto const xf = analyzeC("long double xf __attribute__((mode(XF)));\n",
                                 DataModel::Lp64, LongDoubleFormat::Ieee128);
        EXPECT_EQ(countCode(xf.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 1u)
            << everyDiagnostic(xf);
    }
    {   // `long double` IS `double` (pe64, Apple arm64): both are refused.
        for (char const* src : {"double xf __attribute__((mode(XF)));\n",
                                "double tf __attribute__((mode(TF)));\n"}) {
            auto const m = analyzeC(src, DataModel::Lp64, LongDoubleFormat::F64);
            EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 1u)
                << src << everyDiagnostic(m);
        }
    }
}

// ── POINTERS AND ENUMERATIONS ──────────────────────────────────────────────

TEST(MachineModeAttribute, APointerKeepsItsTypeAtItsOwnWidthAndIsRefusedAtAnother) {
    {
        auto const m = analyzeC("int *pm __attribute__((mode(DI)));\n"
                                "int * __attribute__((mode(pointer))) pp;\n");
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        TypeInterner const& in = m.lattice().interner();
        EXPECT_EQ(in.kind(typeOf(m, "pm")), TypeKind::Ptr);
        EXPECT_EQ(in.kind(typeOf(m, "pp")), TypeKind::Ptr);
        EXPECT_EQ(sizeOf(m, "pm"), 8u);
    }
    auto const m = analyzeC("int *ps __attribute__((mode(SI)));\n");
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 1u)
        << everyDiagnostic(m);
}

// A mode after an enumeration's body sets the enumeration's width; a typedef of an
// enumeration type, and an object of one, take theirs as any declared type does.
//
// RED-ON-DISABLE: drop the completion's mode arm and `enum E` stays 4 bytes.
TEST(MachineModeAttribute, AnEnumerationTakesTheWidthOfItsMode) {
    auto const m = analyzeC(
        "enum E { EA = 1, EB = 2 } __attribute__((mode(QI)));\n"
        "enum E e;\n"
        "typedef enum F { FA = 1 } Ft __attribute__((mode(DI)));\n"
        "Ft ft;\n"
        "enum H { HA = 1, HB = 2 };\n"
        "enum H hv __attribute__((mode(HI))) = HB;\n");
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    EXPECT_TRUE(m.diagnostics().all().empty()) << everyDiagnostic(m);
    EXPECT_EQ(sizeOf(m, "e"), 1u);
    EXPECT_EQ(sizeOf(m, "ft"), 8u);
    EXPECT_EQ(sizeOf(m, "hv"), 2u);
}

// An enumerator the mode's width cannot hold is refused (gcc's answer; clang
// converts the value), and a FIXED underlying type cannot be given a second width.
TEST(MachineModeAttribute, AnEnumerationsModeThatCannotHoldItsValuesIsRefused) {
    for (char const* src : {
             "enum G { GA = 1, GB = 300 } __attribute__((mode(QI)));\n",
             "enum K : int { KA = 1 } __attribute__((mode(QI)));\n"}) {
        auto const m = analyzeC(src);
        EXPECT_TRUE(m.hasErrors()) << src;
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 1u)
            << src << everyDiagnostic(m);
    }
}

// ── THE REFUSALS ───────────────────────────────────────────────────────────

// What every reference refuses is refused BY NAME, exactly once, and never also
// called unknown: the attribute is known, its operand or its type is not one it
// can take. A vector mode is refused too: this compiler has no vector types. A
// declaration-level mode that reaches two declarators is refused once.
TEST(MachineModeAttribute, WhatEveryReferenceRefusesIsRefusedByNameOnce) {
    for (char const* src : {
             "typedef int Bad __attribute__((mode(XYZ)));\n",
             "typedef int Bare __attribute__((mode));\n",
             "typedef int Str __attribute__((mode(\"DI\")));\n",
             "typedef int V4 __attribute__((mode(V4SI)));\n",
             "typedef int IntAsSF __attribute__((mode(SF)));\n",
             "typedef float FloatAsDI __attribute__((mode(DI)));\n",
             "int arr[2] __attribute__((mode(DI)));\n",
             "int fn(void) __attribute__((mode(DI)));\n",
             "__attribute__((mode(DI))) int lead(void);\n",
             "struct S { int a; } __attribute__((mode(QI)));\n",
             "__attribute__((mode(XYZ))) int a, b;\n"}) {
        auto const m = analyzeC(src);
        EXPECT_TRUE(m.hasErrors()) << src;
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 1u)
            << src << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_UnknownAttribute), 0u)
            << "refused, not also called unknown: " << src << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_AttributeIgnoredForDeclarationKind),
                  0u)
            << "refused, not also called ignored: " << src << everyDiagnostic(m);
    }
}

// A mode where nothing is declared — in a type name — is refused by name: a mode
// is applied to a declared type, and a size nobody asked for is never left
// standing in silence.
TEST(MachineModeAttribute, AModeWhereNothingIsDeclaredIsRefusedByName) {
    auto const m = analyzeC("char a[sizeof(__attribute__((mode(DI))) int)];\n");
    EXPECT_TRUE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 1u)
        << everyDiagnostic(m);
}

// ── THE TABLE IS THE LANGUAGE DOCUMENT'S, AND ITS READER REFUSES A BAD ONE ─

TEST(MachineModeAttribute, TheTableReaderRefusesAMalformedTable) {
    struct Edit { char const* from; char const* to; char const* needle; };
    constexpr Edit kEdits[] = {
        {R"("machineModes": [)", R"("$machineModes": [)", "'machineModes' is REQUIRED"},
        {R"("effect": "machineMode", "appliesTo": ["variable", "type", "function"],)",
         R"("effect": "warnOnUse", "appliesTo": ["variable", "type", "function"],)",
         "'machineModes' belongs to the 'machineMode' verb's row"},
        {R"({ "names": ["HI"], "integerBytes": 2 })",
         R"({ "names": ["HI"], "integerBytes": 2, "core": "F32" })", "EXACTLY ONE"},
        {R"({ "names": ["HI"], "integerBytes": 2 })", R"({ "names": ["HI"] })",
         "EXACTLY ONE"},
        {R"({ "names": ["HI"], "integerBytes": 2 })",
         R"({ "names": ["HI"], "integerBytes": 3 })", "'integerBytes' must be"},
        {R"({ "names": ["SF"], "core": "F32" })", R"({ "names": ["SF"], "core": "I32" })",
         "'core' must name a floating representation"},
        {R"({ "names": ["HI"], "integerBytes": 2 })",
         R"({ "names": ["QI"], "integerBytes": 2 })", "is named by two"},
        {R"({ "names": ["HI"], "integerBytes": 2 })",
         R"({ "names": ["__HI__"], "integerBytes": 2 })", "surrounding double underscores"},
        {R"({ "names": ["HI"], "integerBytes": 2 })",
         R"({ "names": ["HI"], "integerBytes": 2, "width": 2 })", "unknown key 'width'"},
    };
    for (Edit const& e : kEdits) {
        std::string const errors = loadErrorsOfEditedC(e.from, e.to, 1);
        EXPECT_FALSE(errors.empty()) << "the edited document must be refused: " << e.to;
        EXPECT_NE(errors.find(e.needle), std::string::npos)
            << "edit " << e.to << " must be refused saying '" << e.needle
            << "'; the load said:\n"
            << errors;
    }
    // CONTROL: an edit that keeps the table well formed loads.
    EXPECT_TRUE(loadErrorsOfEditedC(R"({ "names": ["HI"], "integerBytes": 2 })",
                                    R"({ "names": ["HI", "half"], "integerBytes": 2 })", 1)
                    .empty());
}
