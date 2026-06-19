#pragma once

// Shared test fixture: lower a snippet of c-subset source all the way
// to LIR, threading each phase's diagnostics. Used by liveness +
// regalloc tests to exercise the substrate end-to-end without
// re-rolling the pipeline boilerplate in each TU.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
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
// substrate) hands the pre-built schema in directly; the c-subset →
// HIR → MIR half of the pipeline is target-independent, so only the
// MIR→LIR step consumes it.
[[nodiscard]] inline LoweredLir
lowerCSubsetToLir(std::string src, std::shared_ptr<TargetSchema> target,
                  std::uint16_t mirCcIndex = 0) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) { ADD_FAILURE() << "loadShipped(c-subset) failed"; std::abort(); }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu    = std::make_shared<CompilationUnit>(std::move(builder).finish());
    if (target == nullptr) {
        ADD_FAILURE() << "lowerCSubsetToLir: null target schema";
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
    auto model = analyze(cu, DataModel::Lp64, std::nullopt, vaStrategy);
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
        mirCfg.vaListLayout              = cc->vaListLayout;
    }
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter,
                                    &hir->sourceMap, mirCfg);
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
lowerCSubsetToLir(std::string src, std::string targetName = "x86_64",
                  std::uint16_t mirCcIndex = 0) {
    auto target = TargetSchema::loadShipped(targetName);
    if (!target) {
        ADD_FAILURE() << "loadShipped(" << targetName << ") failed";
        std::abort();
    }
    return lowerCSubsetToLir(std::move(src), std::move(*target), mirCcIndex);
}

} // namespace dss::test_support
