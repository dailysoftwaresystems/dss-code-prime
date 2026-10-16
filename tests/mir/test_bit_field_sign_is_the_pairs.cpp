// ══ A BIT-FIELD'S SIGN IS THE PAIR'S, IN BOTH TIERS: A PLAIN `char` FIELD AND AN ENUMERATION FIELD ══
// (cycle P69 round 1, lane `lm`; a review of checkpoint 21, its findings F1 and N2.)
//
// Whether a plain `char` is signed is the target's (C 6.2.5p15): signed on x86_64 and on
// Apple's arm64, unsigned on arm64 GNU/Linux — one key of the target document, read
// through `TargetSchema::charIsUnsigned(ObjectFormatKind)`. A `char` BIT-FIELD has that
// sign too: ✔MEASURED 2026-10-15, `char b : 3` holding 7 reads -1 on gcc 13, clang 18,
// AppleClang 21, mingw-w64 gcc 13.2 and cl 19.51, and 7 on gcc 13 for arm64 GNU/Linux.
//
// The constant tier read the field by that sign (`readThroughBitField`, through
// `classifyCstCastTarget`); the run time did not — its extraction asked a list of its own
// (I8..I128, no `Char`) and read every `char` field UNSIGNED. On a signed-char target the
// two tiers disagreed, and the disagreement was not only a wrong value: `if (g.b == -1)
// return 42; return 7;` over a constant `g` folded TRUE, what follows the `if` was marked
// never reached, the run time read 7 and walked into the mark (✔MEASURED, pe64, both
// pipelines: the image died of an illegal instruction). The extraction asks the ONE
// target-aware sign now (`isSignedIntKind`, `src/mir/lowering/hir_to_mir.cpp`).
//
// AN ENUMERATION FIELD has the sign of the integer type its enumeration is compatible
// with, which the OBJECT FORMAT's `enumCompatibleTypeRule` chooses (the Microsoft x64
// ABI's `int`; the SysV / AAPCS / Darwin `unsigned int` when no value is negative):
// ✔MEASURED the same day, `enum E { EA, EB }` field `e : 3` holding 7 reads -1 on cl and
// 7 on gcc, clang, AppleClang, mingw-w64 gcc and arm64 gcc — the one pair the references
// split on is pe64 (cl -1, mingw-w64 gcc 7), and DSS reads it as the format's documented
// rule says (Microsoft's, `int`).
//
// Pinned here through the driver's own derivation of the pair (`applyTargetFormatPair`,
// `analyzeForTargetFormat`), on four pairs, in both tiers: the constant read's sign by a
// static assertion that holds on every pair and its negative twin, which fails on every
// one; the run-time read's sign by the shift the extraction is built with (an arithmetic
// right shift exactly when the field is signed). The values are run by
// `examples/c/bit_field_sign_is_the_pairs`.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/target_format_analysis.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "diagnostic_count.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "link/object_format_schema.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace dss;
using dss::test_support::countCode;

namespace {

struct Row {
    char const* target;
    char const* format;
    bool        charIsSigned;   // the pair's plain `char`, as its references read it
    bool        enumIsSigned;   // an enumeration with no negative value, likewise
};

constexpr Row kRows[] = {
    {"x86_64", "pe64-x86_64-windows-exec", true, true},
    {"x86_64", "elf64-x86_64-linux-exec", true, false},
    {"arm64", "elf64-aarch64-linux-exec", false, false},
    {"arm64", "macho64-arm64-darwin-exec", true, false},
};

struct Lowered {
    std::shared_ptr<TargetSchema const>       target;   // first: the model republishes it
    std::shared_ptr<ObjectFormatSchema const> format;
    SemanticModel                             model;
    std::unique_ptr<CstToHirResult>           hir;
    DiagnosticReporter                        hirReporter;
    HirToMirResult                            mir;
    DiagnosticReporter                        mirReporter;
};

// The source through the driver's analysis of the pair, HIR and MIR — the MIR told the
// pair's facts the way the pipeline tells them (the format's bit-field strategy, its data
// model, the target's plain-char answer for the format's kind).
[[nodiscard]] Lowered lowerFor(std::string const& src, Row const& row) {
    auto const schema = dss::test_support::shippedSchemaOrThrow("c");
    auto loadedTarget = TargetSchema::loadShipped(row.target);
    if (!loadedTarget.has_value()) throw std::runtime_error(std::string{"target "} + row.target);
    auto loadedFormat = ObjectFormatSchema::loadShipped(row.format);
    if (!loadedFormat) throw std::runtime_error(std::string{"format "} + row.format);
    std::shared_ptr<TargetSchema const> const       target = *loadedTarget;
    std::shared_ptr<ObjectFormatSchema const> const format = *loadedFormat;
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    applyTargetFormatPair(builder, *target, *format);
    builder.addInMemory(src, "<mem>");
    auto cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());
    auto analysis = analyzeForTargetFormat(cu, DiagnosticBudget::libraryDefault(), *target,
                                           *format, nullptr);
    SemanticModel model = std::move(analysis.model);
    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);
    DiagnosticReporter mirReporter;
    MirLoweringConfig mirCfg = languageMirLoweringConfig(*schema);
    mirCfg.symbolIdSpace = MirSymbolIdSpace::endingAt(
        static_cast<std::uint32_t>(model.symbols().size()));
    mirCfg.aggregateLayout                          = target->aggregateLayout();
    mirCfg.aggregateLayout.bitFieldStrategy         = effectiveBitFieldStrategy(*target, *format);
    mirCfg.aggregateLayout.unnamedBitFieldAlignment =
        effectiveUnnamedBitFieldAlignment(*target, *format);
    mirCfg.aggregateLayoutLoaded = target->aggregateLayoutLoaded();
    mirCfg.dataModel             = format->dataModel();
    mirCfg.charIsUnsigned        = target->charIsUnsigned(format->kind());
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool, model.lattice().interner(),
                                    mirReporter, &hir->sourceMap, mirCfg, /*ffiMap=*/nullptr,
                                    &hir->linkageMap, &hir->mutabilityMap, &hir->volatileMap);
    return Lowered{target, format, std::move(model), std::move(hir), std::move(hirReporter),
                   std::move(mir), std::move(mirReporter)};
}

[[nodiscard]] std::string actuals(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all()) out += "\n    " + d.actual;
    return out;
}

[[nodiscard]] std::size_t countOp(Mir const& m, MirOpcode op) {
    std::size_t n = 0;
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi) {
        MirFuncId const f = m.funcAt(fi);
        for (std::uint32_t bi = 0; bi < m.funcBlockCount(f); ++bi) {
            MirBlockId const b = m.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < m.blockInstCount(b); ++ii)
                if (m.instOpcode(m.blockInstAt(b, ii)) == op) ++n;
        }
    }
    return n;
}

// Fields holding the bit pattern 111: a plain `char`, a `signed char`, an `unsigned char`,
// an enumeration with no negative value and one with a negative value. A constant object
// for the constant tier, a volatile one for the run time.
constexpr char const* kObjects =
    "enum E { EA, EB };\n"
    "enum N { NM = -1, NP = 1 };\n"
    "struct S { char b : 3; signed char s : 3; unsigned char u : 3; };\n"
    "struct T { enum E e : 3; enum N n : 3; };\n"
    "static const struct S g = { 7, 7, 7 };\n"
    "static volatile struct S v = { 7, 7, 7 };\n"
    "static const struct T k = { 7, 7 };\n"
    "static volatile struct T t = { 7, 7 };\n";

// The source with `asserts` at file scope and a function that reads `field` of a
// volatile object (`v.b`, `t.e`, ...) — the program's only read of a field at run time.
[[nodiscard]] std::string programReading(char const* field, char const* asserts) {
    return std::string{kObjects} + asserts + "int r(void) { return " + field + "; }\n";
}

struct Field {
    char const* read;
    bool (*isSigned)(Row const&);
};

constexpr Field kFields[] = {
    {"v.b", [](Row const& r) { return r.charIsSigned; }},
    {"v.s", [](Row const&) { return true; }},
    {"v.u", [](Row const&) { return false; }},
    {"t.e", [](Row const& r) { return r.enumIsSigned; }},
    {"t.n", [](Row const&) { return true; }},
};

} // namespace

// THE CONSTANT TIER: a constant's plain `char` field is negative exactly where the pair's
// plain `char` is signed, and its enumeration field exactly where the enumeration is
// compatible with a signed type — and the twins that say the opposite fail on every
// pair, so the first are held by the reads and not by assertions that cannot fail.
TEST(BitFieldSignIsThePairs, TheConstantReadTakesThePairsSign) {
    for (Row const& row : kRows) {
        SCOPED_TRACE(std::string{row.target} + " / " + row.format);
        Lowered const holds = lowerFor(programReading("v.u",
            "_Static_assert((g.b < 0) == ((char)-1 < 0), \"the pair's char\");\n"
            "_Static_assert((k.e < 0) == ((enum E)-1 < 0), \"the pair's enumeration\");\n"
            "_Static_assert(g.s < 0 && k.n < 0, \"signed on every pair\");\n"
            "_Static_assert(g.u == 7, \"unsigned on every pair\");\n"), row);
        EXPECT_EQ(holds.model.diagnostics().errorCount(), 0u) << actuals(holds.model.diagnostics());
        Lowered const twin = lowerFor(programReading("v.u",
            "_Static_assert((g.b < 0) != ((char)-1 < 0), \"the opposite char\");\n"
            "_Static_assert((k.e < 0) != ((enum E)-1 < 0), \"the opposite enumeration\");\n"), row);
        EXPECT_EQ(countCode(twin.model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 2u)
            << actuals(twin.model.diagnostics());
        EXPECT_EQ(twin.model.diagnostics().errorCount(), 2u) << actuals(twin.model.diagnostics());
        // The premise, on the pair itself: its plain `char` and its enumeration are signed
        // as the references read them.
        Lowered const premise = lowerFor(programReading("v.u",
            row.charIsSigned ? "_Static_assert((char)-1 < 0, \"\");\n"
                             : "_Static_assert((char)-1 > 0, \"\");\n"), row);
        EXPECT_EQ(premise.model.diagnostics().errorCount(), 0u) << actuals(premise.model.diagnostics());
        Lowered const enumPremise = lowerFor(programReading("v.u",
            row.enumIsSigned ? "_Static_assert((enum E)-1 < 0, \"\");\n"
                             : "_Static_assert((enum E)-1 > 0, \"\");\n"), row);
        EXPECT_EQ(enumPremise.model.diagnostics().errorCount(), 0u)
            << actuals(enumPremise.model.diagnostics());
    }
}

// THE RUN TIME: a field's extraction is an arithmetic right shift exactly where the field
// is signed — the plain `char` and the enumeration by the pair; `signed char` and the
// enumeration with a negative value on every pair, `unsigned char` on none (the controls).
TEST(BitFieldSignIsThePairs, TheRunTimeReadTakesThePairsSign) {
    for (Row const& row : kRows) {
        SCOPED_TRACE(std::string{row.target} + " / " + row.format);
        for (Field const& f : kFields) {
            SCOPED_TRACE(f.read);
            bool const isSigned = f.isSigned(row);
            Lowered const L = lowerFor(programReading(f.read, ""), row);
            ASSERT_FALSE(L.model.hasErrors()) << actuals(L.model.diagnostics());
            ASSERT_TRUE(L.hir->ok) << actuals(L.hirReporter);
            ASSERT_TRUE(L.mir.ok) << actuals(L.mirReporter);
            EXPECT_EQ(countOp(L.mir.mir, MirOpcode::AShr), isSigned ? 1u : 0u)
                << "the extraction of a " << (isSigned ? "signed" : "unsigned")
                << " field is " << (isSigned ? "" : "not ") << "an arithmetic right shift";
        }
    }
}

// BOTH TIERS ASK ONE ANSWER: on each pair, the constant tier's reading of a field and the
// run time's are the same sign — the property whose loss deleted the code after
// `if (g.b == -1)`.
TEST(BitFieldSignIsThePairs, TheTwoTiersReadOneSign) {
    struct Twin {
        char const* constant;   // a static assertion that holds when the constant read is negative
        char const* runTime;
    };
    for (Row const& row : kRows) {
        SCOPED_TRACE(std::string{row.target} + " / " + row.format);
        for (Twin const tw : {Twin{"_Static_assert(g.b < 0, \"\");\n", "v.b"},
                              Twin{"_Static_assert(k.e < 0, \"\");\n", "t.e"}}) {
            SCOPED_TRACE(tw.runTime);
            Lowered const constant = lowerFor(programReading("v.u", tw.constant), row);
            bool const constantIsSigned =
                countCode(constant.model.diagnostics(), DiagnosticCode::S_StaticAssertFailed) == 0;
            Lowered const runTime = lowerFor(programReading(tw.runTime, ""), row);
            ASSERT_TRUE(runTime.mir.ok) << actuals(runTime.mirReporter);
            bool const runTimeIsSigned = countOp(runTime.mir.mir, MirOpcode::AShr) == 1;
            EXPECT_EQ(runTimeIsSigned, constantIsSigned)
                << "the run time reads the field " << (runTimeIsSigned ? "signed" : "unsigned")
                << " where the constant tier reads it " << (constantIsSigned ? "signed" : "unsigned")
                << actuals(constant.model.diagnostics());
        }
    }
}
