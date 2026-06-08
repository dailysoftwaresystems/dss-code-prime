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

// `targetName` defaults to "x86_64" (the historic behavior — every
// pre-existing caller is unaffected). A test that needs the lowering
// against a different target's calling convention (e.g. ARM64's
// AAPCS64 link-register frame discipline) passes the target name
// explicitly; the whole lower->LIR pipeline is target-parameterized.
[[nodiscard]] inline LoweredLir
lowerCSubsetToLir(std::string src, std::string targetName = "x86_64") {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) { ADD_FAILURE() << "loadShipped(c-subset) failed"; std::abort(); }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu    = std::make_shared<CompilationUnit>(std::move(builder).finish());
    auto model = analyze(cu);
    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);
    DiagnosticReporter mirReporter;
    MirLoweringConfig mirCfg;
    mirCfg.globalsAllowFloat = (*loaded)->hirLowering().globalsConstEval.allowFloat;
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter,
                                    &hir->sourceMap, mirCfg);
    auto target = TargetSchema::loadShipped(targetName);
    if (!target) {
        ADD_FAILURE() << "loadShipped(" << targetName << ") failed";
        std::abort();
    }
    DiagnosticReporter lirReporter;
    MirToLirResult lir = lowerToLir(mir.mir, **target,
                                    model.lattice().interner(),
                                    lirReporter);
    return LoweredLir{std::move(model), std::move(hir), std::move(hirReporter),
                      std::move(mir), std::move(mirReporter), std::move(*target),
                      std::move(lirReporter), std::move(lir)};
}

} // namespace dss::test_support
