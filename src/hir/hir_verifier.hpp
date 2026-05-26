#pragma once

#include "core/export.hpp"
#include "hir/hir.hpp"

namespace dss {

class DiagnosticReporter;

// HIR structural verifier (HR2 — first slice; the full invariant set lands at
// HR6 per plan §2.8). A verifier is constructed over a frozen `Hir` and run
// against a `DiagnosticReporter`; each structural rule is a private method
// `verify()` calls in turn. HR2 ships one rule — `checkExpressionTypes` — and
// HR6 grows the class by adding sibling rule methods (break/continue scoping,
// Call-arg-vs-FnSig, intrinsic-registered, shader restrictions) to the same
// `verify()` body.
//
// Discipline (mirrors the semantic analyzer): COLLECT-ALL, never short-circuit —
// every node is checked so one run surfaces every violation. Violations are
// recoverable diagnostics (`reporter.report`), NOT aborts: an untyped expression
// is a diagnosable lowering/analysis outcome, not a compiler-internal invariant
// breach (those still abort at the substrate/builder layer).
class DSS_EXPORT HirVerifier {
public:
    explicit HirVerifier(Hir const& hir) noexcept : hir_(hir) {}

    // The verifier stores a reference and must not outlive the module it
    // inspects — forbid binding to a temporary `Hir` outright.
    HirVerifier(Hir&&) = delete;

    // Run every HR2 rule, reporting each violation into `reporter`. Returns true
    // iff THIS run emitted no Error-severity diagnostic (computed by delta on the
    // reporter's error count, so a reporter carrying prior-phase errors doesn't
    // make a clean module look dirty).
    [[nodiscard]] bool verify(DiagnosticReporter& reporter) const;

private:
    // Every node whose kind `requiresValidType` (the Expressions group + TypeRef)
    // must carry a `typeId.valid()`. A node flagged `HasError` is skipped
    // (cascade suppression). Each miss emits `H_TypeUnresolved`.
    void checkExpressionTypes(DiagnosticReporter& reporter) const;

    Hir const& hir_;
};

} // namespace dss
