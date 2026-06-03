#include "opt/optimizer.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <format>

// OPT1 cycle 1: pass-engine entry point. Cycle 1 ships ONLY the
// identity (no-op) pass. The function exists so future cycles can
// land actual passes (OPT2 const-fold / DCE / copy-prop / peephole)
// without churning every compile-pipeline call site.

namespace dss::opt {

namespace {

// Per-pass dispatcher. Cycle 1: only Identity, which is a no-op.
// Returns true on success; false on any pass-level failure (cycle
// 1's only failure mode is the enum-drift fallback).
//
// Code-reviewer post-fold C2 (2026-06-03): the enum-drift fallback
// EMITS a loud `X_UnknownPassId` diagnostic before returning false.
// Pre-fix it returned false silently — `tierClean` in the compile
// pipeline observed no new errors and the outer pipeline saw a
// silent failure. Closes D-OPT1-PASS-ID-STABILITY's enforcement.
[[nodiscard]] bool runPass(PassId id, Mir& /*mir*/,
                           TargetSchema const& /*target*/,
                           DiagnosticReporter& reporter) {
    switch (id) {
        case PassId::Identity:
            // No-op. The pass exists to exercise the engine's
            // end-to-end wiring + the future verify-after-pass
            // harness without yet mutating any MIR.
            return true;
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
    // D-OPT1-RETURN-FALSE-DIAGNOSTIC-CONTRACT (silent-failure post-
    // fold F3 2026-06-03): `optimize()` returning false MUST imply
    // `reporter.errorCount() > entry`. The contract is enforced
    // structurally by `runPass` (which is the only failure path
    // and which emits X_UnknownPassId before returning false); the
    // snapshot+assertion belt-and-suspenders below catches a future
    // failure path that forgets to report. Pre-fix, the compile
    // pipeline's tierClean(reporter, optEntry) check would observe
    // unchanged errorCount, and the outer pipeline would exit
    // without any diagnostic surfaced — a silent compilation
    // failure.
    auto const entryErrorCount = reporter.errorCount();

    // Cycle 1: pipelines run the Identity (no-op) pass at minimum,
    // so the loop ALWAYS executes at least once. Once the first
    // non-Identity pass lands, this loop also runs the verify-
    // after-every-pass harness (D-OPT1-VERIFY-AFTER-EVERY-PASS —
    // anchored).
    for (PassId p : pipeline.passes) {
        if (!runPass(p, mir, target, reporter)) {
            // Contract: a false return MUST be paired with at
            // least one new error. runPass's switch arms guarantee
            // this; the assertion makes a future regression loud.
            if (reporter.errorCount() <= entryErrorCount) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::X_UnknownPassId;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = "opt::optimize: pass returned false WITHOUT "
                             "emitting a diagnostic — substrate contract "
                             "violation (D-OPT1-RETURN-FALSE-DIAGNOSTIC-"
                             "CONTRACT). Every false-return path MUST "
                             "report at least one error before returning.";
                reporter.report(std::move(d));
            }
            return false;
        }
        // Anchored D-OPT1-VERIFY-AFTER-EVERY-PASS: cycle 1's
        // identity pass cannot break the MIR invariants (it
        // mutates nothing), so the verifier is not run here yet.
        // OPT2's first real pass lands the unconditional
        // verifier hook in this exact slot.
    }
    return true;
}

} // namespace dss::opt
