#include "opt/optimizer.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "mir/mir_verifier.hpp"
#include "opt/passes/const_fold.hpp"
#include "opt/passes/copy_prop.hpp"
#include "opt/passes/cse.hpp"
#include "opt/passes/dce.hpp"
#include "opt/passes/licm.hpp"
#include "opt/passes/mem2reg.hpp"
#include "opt/passes/simplify_cfg.hpp"

#include <format>

// Per-pass dispatcher. `runPass` returns:
//   { ok: bool, mutated: bool }
// On `ok=false` it MUST have emitted at least one diagnostic
// (D-OPT1-RETURN-FALSE-DIAGNOSTIC-CONTRACT — belt-and-suspenders
// guard in `optimize()` below).

namespace dss::opt {

namespace {

struct PassRunResult {
    bool ok      = false;
    bool mutated = false;
};

[[nodiscard]] PassRunResult runPass(PassId id, Mir& mir,
                                    TargetSchema const& /*target*/,
                                    TypeInterner const& interner,
                                    DiagnosticReporter& reporter) {
    switch (id) {
        case PassId::Identity:
            return {true, false};  // no-op; exercises the engine wiring.
        case PassId::ConstFold: {
            auto const r = passes::runConstFold(mir, interner, reporter);
            return {r.ok, r.instructionsFolded > 0};
        }
        case PassId::Dce: {
            auto const r = passes::runDce(mir, interner, reporter);
            return {r.ok,
                    r.instructionsEliminated + r.blocksEliminated
                  + r.functionsEliminated   + r.globalsEliminated > 0};
        }
        case PassId::Mem2Reg: {
            auto const r = passes::runMem2Reg(mir, interner, reporter);
            return {r.ok,
                    r.allocasPromoted + r.phisInserted
                  + r.loadsReplaced  + r.storesEliminated > 0};
        }
        case PassId::CopyProp: {
            auto const r = passes::runCopyProp(mir, interner, reporter);
            return {r.ok, r.phisCollapsed > 0};
        }
        case PassId::Cse: {
            auto const r = passes::runCse(mir, interner, reporter);
            return {r.ok, r.instructionsCsed > 0};
        }
        case PassId::SimplifyCfg: {
            auto const r = passes::runSimplifyCfg(mir, interner, reporter);
            return {r.ok, r.branchesFolded + r.blocksJumpThreaded > 0};
        }
        case PassId::Licm: {
            auto const r = passes::runLicm(mir, interner, reporter);
            return {r.ok, r.instructionsHoisted > 0};
        }
    }
    // Enum-drift fallback. A future PassId enumerator added without
    // a matching switch arm above would silently no-op without the
    // explicit fail-loud below. The static_assert on kPassIdCount
    // catches this at compile time too; the runtime emit is the
    // belt-and-suspenders for a third-party adding an enumerator
    // via reinterpret_cast or unchecked numeric construction.
    ParseDiagnostic d;
    d.code     = DiagnosticCode::X_UnknownPassId;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::format(
        "opt::runPass: PassId ordinal {} has no dispatch arm — "
        "substrate-shape violation (D-OPT1-PASS-ID-STABILITY).",
        static_cast<int>(id));
    reporter.report(std::move(d));
    return {false, false};
}

} // namespace

OptResult optimize(Mir& mir,
                   TargetSchema const& target,
                   TypeInterner const& interner,
                   OptPipeline const& pipeline,
                   DiagnosticReporter& reporter) {
    // D-OPT1-RETURN-FALSE-DIAGNOSTIC-CONTRACT: a false return MUST
    // be paired with a new error. Snapshot + belt-and-suspenders
    // emit below covers any future failure path that forgets to.
    auto const entryErrorCount = reporter.errorCount();

    OptResult result{};

    // Symmetric defense: the JSON loader rejects empty `passes` at
    // load time, but a caller constructing OptPipeline{} directly in
    // code (test fixtures, future programmatic builders, the
    // CompileConfig→pipeline mapping when it lands) could bypass that
    // check and silently produce an "optimizer ran nothing" result
    // observationally indistinguishable from a successful run. Reject
    // at the engine entrypoint too — use [Identity] for explicit no-op.
    if (pipeline.passes.empty()) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::X_PipelineMalformed;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = "opt::optimize: pipeline has zero passes — substrate "
                     "contract violation. Use [Identity] for an explicit "
                     "no-op pipeline.";
        reporter.report(std::move(d));
        return result;
    }

    // Pipeline-level fixed-point loop (D-OPT-FIXED-POINT-LOOP +
    // D-OPT1-PASS-RUN-MAX-ITER). The whole `passes` sequence reruns
    // up to `maxIterations` times, or stops early when a full
    // iteration produces zero `passesMutated` (the fixed-point
    // signal). Each pass remains internally idempotent; only the
    // MUTUALLY-ENABLING cluster (ConstFold ↔ SimplifyCfg ↔ Dce)
    // needs the outer loop. `maxIterations = 1` (the default)
    // preserves single-pass semantics for every pipeline that
    // doesn't opt in.
    // Defense — loader rejects 0; clamp here too so a programmatic
    // OptPipeline construction (test fixture, future builder API)
    // bypassing the loader still gets at-least-one iteration.
    std::uint8_t const maxIter = pipeline.maxIterations == 0
        ? std::uint8_t{1}
        : pipeline.maxIterations;
    // Pessimistic default: stays `false` if we exit the loop without
    // observing a mutation-free iteration. Set `true` on the first
    // converged iteration.
    result.fixedPointReached = false;
    for (std::uint8_t iter = 0; iter < maxIter; ++iter) {
        std::size_t const mutatedAtIterStart = result.passesMutated;
        for (PassId p : pipeline.passes) {
            auto const passResult = runPass(p, mir, target, interner, reporter);
            ++result.passesRun;
            if (!passResult.ok) {
                if (reporter.errorCount() <= entryErrorCount) {
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::X_OptReturnFalseWithoutDiagnostic;
                    d.severity = DiagnosticSeverity::Error;
                    d.actual   = "opt::optimize: pass returned ok=false WITHOUT "
                                 "emitting a diagnostic — substrate contract "
                                 "violation (D-OPT1-RETURN-FALSE-DIAGNOSTIC-"
                                 "CONTRACT).";
                    reporter.report(std::move(d));
                }
                return result;
            }
            if (passResult.mutated) {
                ++result.passesMutated;
                // Per-pass effectiveness signal (D-OPT-PASS-METRICS):
                // record each iteration where this PassId mutated.
                // Consumed by effectiveness tests asserting that a
                // pass fired enough times to prove the
                // mutually-enabling cluster converged (e.g.
                // `passMutationCount[ConstFold] >= 2` proves the
                // re-fold post-Mem2Reg happened).
                //
                // The `kPassIdCount` static_assert at the enum
                // declaration site keeps this index in range; an
                // out-of-range `p` getting past `runPass` would also
                // have already routed through the `X_UnknownPassId`
                // fail-loud arm + early-returned. Reaching here with
                // an OOR ordinal is a substrate-contract violation —
                // fail loud rather than silently drop the count.
                auto const idx = static_cast<std::size_t>(p);
                if (idx >= kPassIdCount) {
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::X_UnknownPassId;
                    d.severity = DiagnosticSeverity::Error;
                    d.actual   = std::format(
                        "opt::optimize: PassId ordinal {} bypassed the "
                        "runPass enum-drift guard AND reached the "
                        "passMutationCount increment with mutated=true "
                        "— substrate-shape violation "
                        "(D-OPT1-PASS-ID-STABILITY).",
                        static_cast<int>(p));
                    reporter.report(std::move(d));
                    return result;
                }
                ++result.passMutationCount[idx];
            }

            // D-OPT1-VERIFY-AFTER-EVERY-PASS — unconditional MIR
            // verify after EVERY successful pass under all build
            // modes (plan 22 §3 PR1 directive). A pass that produces
            // an invalid MIR must be a build break, not a runtime
            // miscompile cascading through LIR + asm + linker.
            MirVerifier verifier{mir, &interner};
            if (!verifier.verify(reporter)) {
                return result;
            }
        }
        // Fixed-point check: a full iteration with zero passes-mutated
        // means no remaining transformation enables another.
        if (result.passesMutated == mutatedAtIterStart) {
            result.fixedPointReached = true;
            break;
        }
    }

    result.ok = (reporter.errorCount() == entryErrorCount);
    return result;
}

} // namespace dss::opt
