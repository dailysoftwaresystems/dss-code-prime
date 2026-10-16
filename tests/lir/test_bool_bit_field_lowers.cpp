// ══ A `_Bool` BIT-FIELD IS READ AND WRITTEN ═════════════════════════════════════════
// (cycle P69 round 1, lane `lm`.)
//
// `_Bool` is a bit-field type (C 6.7.2.1p5). A field's read and write go through
// `emitBitfieldExtract` / `emitBitfieldInsert` (src/mir/lowering/hir_to_mir.cpp), which
// compute a unit narrower than 32 bits at `int` and then narrowed the result back to the
// field's type with a `Trunc` — to `_Bool`, a conversion no target realizes (a `_Bool` is
// made by a comparison, C 6.3.1.2). ✔MEASURED 2026-10-15 (pe64, both pipelines): every
// read and every write of a `_Bool b : 1` was refused, `L_UnsupportedLoweringForOpcode`
// "Trunc result: TypeKind ordinal 0", in each of 12 programs gcc 13, clang 18,
// AppleClang 21 and mingw-w64 gcc 13.2 build and run.
//
// Now the read is the field's bit compared with zero (an `ICmpNe` at `_Bool`), and the
// write merges the unit as the byte it is (a `u8` load and store through the unit's
// address re-typed to the byte) — the byte holds the neighbours packed beside the field,
// so it is no `_Bool` value. Pinned here on the two targets' LIR lowerings, and by the
// shape of the MIR each one builds. The values are run by
// `examples/c/bool_bit_field_read_and_written`.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "lir/lir.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace dss;

namespace {

// Read, write, compound write and increment of `_Bool` fields packed beside others.
constexpr char const* kProgram =
    "struct F { unsigned char lo : 3; _Bool f : 1; _Bool g : 1; unsigned char hi : 3; };\n"
    "static volatile struct F v = { 5, 1, 0, 6 };\n"
    "int read(void) { return v.f + v.g; }\n"
    "void write(int x) { v.g = x; v.f = 0; }\n"
    "void bump(void) { v.f++; v.g += 2; }\n"
    "int main(void) { struct F a = { 1, 1, 0, 2 }; a.g = !a.f; write(7); bump();\n"
    "                 return read() + a.g; }\n";

struct Lowered {
    SemanticModel                   model;
    std::unique_ptr<CstToHirResult> hir;
    DiagnosticReporter              hirReporter;
    HirToMirResult                  mir;
    DiagnosticReporter              mirReporter;
    DiagnosticReporter              lirReporter;
};

// The program through HIR and MIR to the LIR of the shipped target `targetName` — the
// pipeline's own assembly of the MIR configuration, as `tests/lir/test_mir_to_lir.cpp`
// states it.
[[nodiscard]] Lowered lowerToTarget(std::string const& targetName,
                                    char const* program = kProgram) {
    auto const loaded = dss::test_support::shippedSchemaOrThrow("c");
    UnitBuilder builder{loaded, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::string{program}, "<mem>");
    auto cu    = std::make_shared<CompilationUnit>(std::move(builder).finish());
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);
    auto target = TargetSchema::loadShipped(targetName);
    if (!target) throw std::runtime_error("loadShipped(" + targetName + ") failed");
    DiagnosticReporter mirReporter;
    MirLoweringConfig mirCfg = languageMirLoweringConfig(*loaded);
    mirCfg.symbolIdSpace = MirSymbolIdSpace::endingAt(
        static_cast<std::uint32_t>(model.symbols().size()));
    mirCfg.aggregateLayout       = (*target)->aggregateLayout();
    mirCfg.aggregateLayoutLoaded = (*target)->aggregateLayoutLoaded();
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter,
                                    &hir->sourceMap, mirCfg, /*ffiMap=*/nullptr,
                                    /*linkageMap=*/nullptr, &hir->mutabilityMap,
                                    /*volatileMap=*/nullptr, &hir->alignmentMap,
                                    /*threadLocalMap=*/nullptr, &hir->vlaSizeExprBySymbol,
                                    /*sizeofVlaSymMap=*/nullptr, /*typedefVlaOriginMap=*/nullptr,
                                    /*synthRecipeMap=*/nullptr, /*returnsTwiceMap=*/nullptr,
                                    /*noInlineMap=*/nullptr, /*alwaysInlineMap=*/nullptr,
                                    /*noOptimizeMap=*/nullptr, /*noSanitizeThreadMap=*/nullptr,
                                    /*inlineAsmPool=*/nullptr, /*inlineDefinitionMap=*/nullptr,
                                    &hir->enclosingFunctionMap);
    DiagnosticReporter lirReporter;
    if (mir.ok) {
        auto lir = lowerToLir(mir.mir, **target, model.lattice().interner(), lirReporter);
        (void)lir;
    }
    return Lowered{
        .model       = std::move(model),
        .hir         = std::move(hir),
        .hirReporter = std::move(hirReporter),
        .mir         = std::move(mir),
        .mirReporter = std::move(mirReporter),
        .lirReporter = std::move(lirReporter),
    };
}

[[nodiscard]] std::string firstActual(DiagnosticReporter const& r) {
    return r.all().empty() ? std::string{} : r.all()[0].actual;
}

void expectLowersClean(std::string const& targetName) {
    Lowered const L = lowerToTarget(targetName);
    ASSERT_FALSE(L.model.hasErrors()) << targetName;
    ASSERT_TRUE(L.hir->ok) << firstActual(L.hirReporter);
    ASSERT_TRUE(L.mir.ok) << firstActual(L.mirReporter);
    EXPECT_EQ(L.lirReporter.errorCount(), 0u)
        << targetName << ": a `_Bool` bit-field's read or write was refused: "
        << firstActual(L.lirReporter);
}

} // namespace

TEST(BoolBitField, ItsReadsAndWritesLowerOnX86_64) { expectLowersClean("x86_64"); }

TEST(BoolBitField, ItsReadsAndWritesLowerOnArm64) { expectLowersClean("arm64"); }

// THE SHAPE: no conversion of the MIR narrows to `_Bool` — the read is a comparison, the
// write stores the byte. A `Trunc` (or any width change) whose result is `_Bool` is the
// form the targets refuse.
TEST(BoolBitField, NothingIsNarrowedToBool) {
    Lowered const L = lowerToTarget("x86_64");
    ASSERT_TRUE(L.mir.ok) << firstActual(L.mirReporter);
    auto const& in = L.model.lattice().interner();
    Mir const& mir = L.mir.mir;
    std::size_t narrowedToBool = 0, comparedToBool = 0;
    for (std::uint32_t i = 1; i < mir.instCount(); ++i) {   // slot 0 is the sentinel
        MirInstId const id{i};
        TypeId const t = mir.instType(id);
        if (!t.valid() || in.kind(t) != TypeKind::Bool) continue;
        MirOpcode const op = mir.instOpcode(id);
        if (op == MirOpcode::Trunc || op == MirOpcode::ZExt || op == MirOpcode::SExt) ++narrowedToBool;
        if (op == MirOpcode::ICmpNe) ++comparedToBool;
    }
    EXPECT_EQ(narrowedToBool, 0u) << "a value narrowed to `_Bool`: the targets refuse it";
    EXPECT_GE(comparedToBool, 1u) << "the premise: a field's read compares its bit with zero";
}

// THE NEIGHBOUR: a field of another narrow type keeps the path it had — its unit computed
// at `int` and narrowed back to the field's own type, which the targets realize.
TEST(BoolBitField, ANarrowFieldOfAnotherTypeIsUnchanged) {
    constexpr char const* kNarrow =
        "struct G { unsigned char a : 3; unsigned char b : 5; };\n"
        "static volatile struct G w = { 5, 17 };\n"
        "int main(void) { w.a = 6; w.b += 1; return w.a + w.b; }\n";
    Lowered const L = lowerToTarget("x86_64", kNarrow);
    ASSERT_TRUE(L.mir.ok) << firstActual(L.mirReporter);
    EXPECT_EQ(L.lirReporter.errorCount(), 0u) << firstActual(L.lirReporter);
    auto const& in = L.model.lattice().interner();
    Mir const& mir = L.mir.mir;
    std::size_t narrowedToByte = 0;
    for (std::uint32_t i = 1; i < mir.instCount(); ++i) {   // slot 0 is the sentinel
        MirInstId const id{i};
        TypeId const t = mir.instType(id);
        if (t.valid() && in.kind(t) == TypeKind::U8 && mir.instOpcode(id) == MirOpcode::Trunc)
            ++narrowedToByte;
    }
    EXPECT_GE(narrowedToByte, 1u) << "the premise: the unit is narrowed back to `unsigned char`";
}
