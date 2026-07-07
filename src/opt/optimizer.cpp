#include "opt/optimizer.hpp"

#include "core/substrate/phase_timers.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "mir/mir_verifier.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "opt/passes/const_fold.hpp"
#include "opt/passes/copy_prop.hpp"
#include "opt/passes/cse.hpp"
#include "opt/passes/dce.hpp"
#include "opt/passes/inlining.hpp"
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
                                    OptPipeline const& pipeline,
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
            return {r.ok,
                    r.branchesFolded + r.blocksJumpThreaded
                  + r.blocksMerged > 0};
        }
        case PassId::Licm: {
            auto const r = passes::runLicm(mir, interner, reporter);
            return {r.ok, r.instructionsHoisted > 0};
        }
        case PassId::Inlining: {
            auto const r = passes::runInlining(mir, interner, reporter,
                                               pipeline.inlineThreshold);
            return {r.ok, r.callsInlined > 0};
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
    // Env-gated per-pass trace (DSS_OPT_TRACE=1). Flushed start/done lines so a
    // KILLED run still shows the pass it hung in — the direct diagnostic for a
    // non-converging fixpoint or a pathological/looping pass on a huge function.
    bool const optTrace = std::getenv("DSS_OPT_TRACE") != nullptr;
    for (std::uint8_t iter = 0; iter < maxIter; ++iter) {
        std::size_t const mutatedAtIterStart = result.passesMutated;
        for (PassId p : pipeline.passes) {
            std::chrono::steady_clock::time_point t0;
            if (optTrace) {
                auto const nm = optPassIdName(p);
                std::fprintf(stderr, "opt: iter=%u pass=%.*s start\n",
                             static_cast<unsigned>(iter),
                             static_cast<int>(nm.size()), nm.data());
                std::fflush(stderr);
                t0 = std::chrono::steady_clock::now();
            }
            auto const passResult =
                runPass(p, mir, target, interner, pipeline, reporter);
            if (optTrace) {
                auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                auto const nm = optPassIdName(p);
                std::fprintf(stderr,
                             "opt: iter=%u pass=%.*s done %lldms mutated=%d\n",
                             static_cast<unsigned>(iter),
                             static_cast<int>(nm.size()), nm.data(),
                             static_cast<long long>(ms),
                             passResult.mutated ? 1 : 0);
                std::fflush(stderr);
            }
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

            // D-OPT1-VERIFY-AFTER-EVERY-PASS / D-OPT1-VERIFY-FREQUENCY-CONFIG —
            // verify after EVERY pass only under the DEVELOPER posture
            // (`verifyEveryPass`, the safe default: LLVM `-verify-each` / GCC
            // `--enable-checking=yes` — pinpoints the pass that produced invalid
            // MIR). The RELEASE posture (verifyEveryPass=false) verifies ONCE
            // after the whole pipeline (below), trusting tested passes — the
            // LLVM/GCC production split. Per-pass verify over a large module is
            // ~passes × iterations full-module verifies (minutes on SQLite).
            // (Sub-scoped as `Verify` so --time separates verify's share from
            // the passes + per-pass rebuild inside the Optimize phase.)
            if (pipeline.verifyEveryPass) {
                substrate::PhaseTimers::Scope verifyScope{
                    substrate::CompilePhase::Verify};
                MirVerifier verifier{mir, &interner};
                if (!verifier.verify(reporter)) {
                    return result;
                }
            }
        }
        // Fixed-point check: a full iteration with zero passes-mutated
        // means no remaining transformation enables another.
        if (result.passesMutated == mutatedAtIterStart) {
            result.fixedPointReached = true;
            break;
        }
    }

    // RELEASE posture (D-OPT1-VERIFY-FREQUENCY-CONFIG): a pipeline that did NOT
    // verify after every pass verifies its FINAL MIR exactly once here — a
    // structurally-broken optimizer output must still be a build break before
    // codegen consumes it, but re-verifying the whole module after every pass
    // over a large module (SQLite) is minutes wasted on a tested pipeline.
    if (!pipeline.verifyEveryPass && reporter.errorCount() == entryErrorCount) {
        substrate::PhaseTimers::Scope verifyScope{substrate::CompilePhase::Verify};
        MirVerifier verifier{mir, &interner};
        if (!verifier.verify(reporter)) {
            return result;
        }
    }

    result.ok = (reporter.errorCount() == entryErrorCount);
    return result;
}

} // namespace dss::opt
