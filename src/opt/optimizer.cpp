#include "opt/optimizer.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "mir/mir_verifier.hpp"
#include "opt/passes/const_fold.hpp"
#include "opt/passes/dce.hpp"

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
    // No fixed-point pass exists in this build; per-pass fixed-point
    // lives on PassRun (D-OPT1-PASS-RUN-MAX-ITER) and will flip this
    // when consumed by a future caller.
    result.fixedPointReached = true;

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
        }

        // D-OPT1-VERIFY-AFTER-EVERY-PASS (live as of OPT2 cycle 1).
        // Unconditional verify after EVERY successful pass — not
        // Debug-only per plan 22 §3 PR1 directive. A pass that
        // produces an invalid MIR must be a build break, not a
        // runtime miscompile cascading through LIR + asm + linker.
        // The verifier's `errorCount` delta-discipline means prior
        // errors don't taint a clean module's result.
        MirVerifier verifier{mir, &interner};
        if (!verifier.verify(reporter)) {
            // Verifier already emitted the I_* diagnostic explaining
            // which invariant was broken. We just exit the loop —
            // a downstream pass running on a broken MIR would
            // cascade more (often confusing) failures.
            return result;
        }
    }

    result.ok = (reporter.errorCount() == entryErrorCount);
    return result;
}

} // namespace dss::opt
