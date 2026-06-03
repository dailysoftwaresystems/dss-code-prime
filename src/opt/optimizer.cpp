#include "opt/optimizer.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <format>

// Per-pass dispatcher. False-return paths MUST emit a diagnostic
// before returning (D-OPT1-RETURN-FALSE-DIAGNOSTIC-CONTRACT).

namespace dss::opt {

namespace {

[[nodiscard]] bool runPass(PassId id, Mir& /*mir*/,
                           TargetSchema const& /*target*/,
                           DiagnosticReporter& reporter) {
    switch (id) {
        case PassId::Identity:
            return true;  // no-op; exercises the engine wiring
    }
    // Enum-drift fallback. A future PassId enumerator added without
    // a matching switch arm above would silently no-op without the
    // explicit fail-loud below.
    ParseDiagnostic d;
    d.code     = DiagnosticCode::X_UnknownPassId;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::format(
        "opt::runPass: PassId ordinal {} has no dispatch arm — "
        "substrate-shape violation (a new PassId enumerator was "
        "added without a matching handler in `runPass`'s switch). "
        "Per D-OPT1-PASS-ID-STABILITY, every enumerator MUST have "
        "an arm OR be guarded by an explicit fallback case.",
        static_cast<int>(id));
    reporter.report(std::move(d));
    return false;
}

} // namespace

bool optimize(Mir& mir,
              TargetSchema const& target,
              OptPipeline const& pipeline,
              DiagnosticReporter& reporter) {
    // D-OPT1-RETURN-FALSE-DIAGNOSTIC-CONTRACT: a false return MUST
    // be paired with a new error. Snapshot + belt-and-suspenders
    // emit below covers any future failure path that forgets to.
    auto const entryErrorCount = reporter.errorCount();

    for (PassId p : pipeline.passes) {
        if (!runPass(p, mir, target, reporter)) {
            // Belt-and-suspenders: every false-return path MUST emit
            // a diagnostic. If runPass returned false without one,
            // emit the contract-violation diagnostic here.
            if (reporter.errorCount() <= entryErrorCount) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::X_UnknownPassId;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = "opt::optimize: pass returned false WITHOUT "
                             "emitting a diagnostic — substrate contract "
                             "violation (D-OPT1-RETURN-FALSE-DIAGNOSTIC-"
                             "CONTRACT).";
                reporter.report(std::move(d));
            }
            return false;
        }
        // D-OPT1-VERIFY-AFTER-EVERY-PASS hook lands here at OPT2's
        // first real pass — see anchor in plan 22 §3.1.
    }
    return true;
}

} // namespace dss::opt
