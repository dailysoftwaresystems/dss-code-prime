#pragma once

// Shared test fixture: lower a snippet of c source all the way
// to LIR, threading each phase's diagnostics. Used by liveness +
// regalloc tests to exercise the substrate end-to-end without
// re-rolling the pipeline boilerplate in each TU.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/lowering/hir_to_mir.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace dss::test_support {

struct LoweredLir {
    SemanticModel                    model;
    std::unique_ptr<CstToHirResult>  hir;
    DiagnosticReporter               hirReporter;
    HirToMirResult                   mir;
    DiagnosticReporter               mirReporter;
    std::shared_ptr<TargetSchema>    target;
    DiagnosticReporter               lirReporter;
    MirToLirResult                   lir;
};

// Schema-injected overload (D-OPT-REGALLOC-EXCLUSION-BUFFER closure,
// 2026-06-11): a test exercising substrate behavior against a
// MUTATED target schema (the tests/test_support/mutate_target_schema
// substrate) hands the pre-built schema in directly; the c →
// HIR → MIR half of the pipeline is target-independent, so only the
// MIR→LIR step consumes it.
[[nodiscard]] inline LoweredLir
lowerCToLir(std::string src, std::shared_ptr<TargetSchema> target,
                  std::uint16_t mirCcIndex = 0) {
    auto loaded = GrammarSchema::loadShipped("c");
    if (!loaded) { ADD_FAILURE() << "loadShipped(c) failed"; std::abort(); }
    UnitBuilder builder{*loaded, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu    = std::make_shared<CompilationUnit>(std::move(builder).finish());
    if (target == nullptr) {
        ADD_FAILURE() << "lowerCToLir: null target schema";
        std::abort();
    }
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): thread the SELECTED CC's va_list
    // strategy into analyze() so the `va_list` TYPE matches the ABI (SysV
    // __va_list_tag[1] vs Win64 char*) — mirrors compile_pipeline.cpp. `mirCcIndex`
    // selects which CC drives BOTH the analyze() strategy AND the MIR config below,
    // so a test can lower a variadic source under ms_x64 (cc 1) end-to-end.
    std::optional<VaListStrategy> vaStrategy;
    if (auto const* cc = target->callingConvention(mirCcIndex);
        cc != nullptr && cc->vaListLayout.has_value()) {
        vaStrategy = cc->vaListLayout->strategy;
    }
    // ★★ INLINE-ASM P5: THE TARGET IS THREADED INTO `analyze`, AND WITHOUT IT AN
    // ASM OPERAND CANNOT REACH LIR AT ALL. A GNU constraint LETTER is a MACHINE
    // fact — `"r"` means whatever the processor's `asmConstraints` facet says —
    // so with no target in scope `HirInlineAsmOperand::regClassResolved` stays
    // false and `lowerInlineAsm` REFUSES rather than guessing a register bank
    // (`… has no resolved register class`, D-CSUBSET-INLINE-ASM-OPERANDS).
    // ⚠ THIS IS THE SAME OMISSION `test_mir_lowering_c.cpp`'s harness had
    // one tier up, and it hides the FEATURE, not just the fixture: every asm
    // source lowered here failed at MIR, so nothing downstream was ever reached.
    // `compile_pipeline.cpp` passes it; a harness that does not cannot test asm.
    // The schema is held by `LoweredLir::target`, which outlives the model —
    // `analyze` takes it NON-OWNING.
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(),
                         DataModel::Lp64, std::nullopt, vaStrategy, std::nullopt,
                         std::nullopt, LongDoubleFormat::None, target.get());
    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);
    DiagnosticReporter mirReporter;
    MirLoweringConfig mirCfg;
    mirCfg.globalsAllowFloat = (*loaded)->hirLowering().globalsConstEval.allowFloat;
    // Thread the SELECTED CC's by-value aggregate + va_list params into MIR
    // lowering, mirroring compile_pipeline.cpp — so a struct-by-value OR a
    // variadic-callee (va_start/va_arg) source lowers through THIS fixture too
    // (mirCcIndex 0 = the target's primary convention: sysv_amd64 on x86_64,
    // aapcs64 on arm64; 1 = ms_x64 on x86_64 for the Win64 variadic pins).
    if (auto const* cc = target->callingConvention(mirCcIndex)) {
        mirCfg.aggregateLayout           = target->aggregateLayout();
        mirCfg.aggregateLayoutLoaded     = target->aggregateLayoutLoaded();
        mirCfg.aggregateClassification   = cc->aggregateClassification;
        mirCfg.aggregateMaxRegBytes      = cc->aggregateMaxRegBytes;
        mirCfg.aggregateSretViaHiddenArg = !cc->indirectResultRegister.has_value();
        mirCfg.argSlotAligned            = cc->slotAligned;
        mirCfg.argGprCount               =
            static_cast<std::uint32_t>(cc->argGprs.size());
        mirCfg.argFprCount               =
            static_cast<std::uint32_t>(cc->argFprs.size());
        mirCfg.aggregateStackExhaustsRegisters =
            cc->aggregateStackExhaustsRegisters;
        // D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED: the stacked-arg
        // packing rules decide `va_start`'s overflow base for a callee whose named
        // params overflow the arg registers. ⚠ THREADED HERE AS PART OF ADDING THE
        // FIELD, not as a follow-up — the docblock below records this fixture
        // silently reading a default THREE times (TF-C78 / TF-C81 / TF-C92) and
        // every test concluding the feature was absent. A default here would make
        // the Apple `va_start` pin PASS against slot packing.
        mirCfg.stackArgPacking           = cc->stackArgPacking;
        mirCfg.vaListLayout              = cc->vaListLayout;
    }
    // D-CSUBSET-VLA (C1b): thread the captured VLA size-expr map so a `int a[n]`
    // source lowers its runtime-operand Alloca through THIS fixture too (else the
    // VLA has no bound → fail loud). The intervening maps stay nullptr (defaults).
    //
    // ★★★ `inlineAsmPool` IS THREADED, AND WITHOUT IT NO LIR TEST CAN SEE AN
    // `__asm__` AT ALL (D-TEST-LIR-FIXTURE-BLIND-TO-INLINE-ASM).
    // `lowerToMir`'s own docblock is explicit that `nullptr` is NOT "no asm" — a
    // descriptor-carrying statement reached with no pool is a loud
    // `H_UnsupportedLoweringForKind`, because the alternative (inventing an
    // operand-less, clobber-less descriptor) is the silent miscompile the P5 arc
    // exists to prevent. So every source with an asm statement failed at MIR here
    // and never reached the tier this directory tests.
    // ⚠ THIS IS THE SAME MISS `test_mir_lowering_c.cpp` RECORDS HITTING
    // THREE TIMES ONE TIER UP (TF-C78 / TF-C81 / TF-C92: a map threaded through
    // the product and through `compile_pipeline.cpp` while every unit test read a
    // nullptr and saw the feature as absent). Treat updating this call as PART OF
    // adding a map, never as a follow-up.
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter,
                                    &hir->sourceMap, mirCfg, /*ffiMap=*/nullptr,
                                    /*linkageMap=*/nullptr, /*mutabilityMap=*/nullptr,
                                    /*volatileMap=*/nullptr, /*alignmentMap=*/nullptr,
                                    /*threadLocalMap=*/nullptr,
                                    &hir->vlaSizeExprBySymbol,
                                    /*sizeofVlaSymMap=*/nullptr,
                                    /*typedefVlaOriginMap=*/nullptr,
                                    /*synthRecipeMap=*/nullptr,
                                    /*returnsTwiceMap=*/nullptr,
                                    /*noInlineMap=*/nullptr,
                                    /*alwaysInlineMap=*/nullptr,
                                    /*noOptimizeMap=*/nullptr,
                                    /*noSanitizeThreadMap=*/nullptr,
                                    &hir->inlineAsmPool,
                                    /*inlineDefinitionMap=*/nullptr,
                                    // D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED:
                                    // without it a `&&label` in a static initializer is
                                    // REFUSED at MIR and never reaches this tier at all
                                    // — the exact shape of the miss the block above
                                    // records hitting three times.
                                    &hir->enclosingFunctionMap);
    DiagnosticReporter lirReporter;
    MirToLirResult lir = lowerToLir(mir.mir, *target,
                                    model.lattice().interner(),
                                    lirReporter);
    return LoweredLir{std::move(model), std::move(hir), std::move(hirReporter),
                      std::move(mir), std::move(mirReporter), std::move(target),
                      std::move(lirReporter), std::move(lir)};
}

// `targetName` defaults to "x86_64" (the historic behavior — every
// pre-existing caller is unaffected). A test that needs the lowering
// against a different target's calling convention (e.g. ARM64's
// AAPCS64 link-register frame discipline) passes the target name
// explicitly; the whole lower->LIR pipeline is target-parameterized.
[[nodiscard]] inline LoweredLir
lowerCToLir(std::string src, std::string targetName = "x86_64",
                  std::uint16_t mirCcIndex = 0) {
    auto target = TargetSchema::loadShipped(targetName);
    if (!target) {
        ADD_FAILURE() << "loadShipped(" << targetName << ") failed";
        std::abort();
    }
    return lowerCToLir(std::move(src), std::move(*target), mirCcIndex);
}

} // namespace dss::test_support
