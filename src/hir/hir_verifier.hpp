#pragma once

#include "core/export.hpp"
#include "hir/hir.hpp"

namespace dss {

class DiagnosticReporter;

// HIR structural verifier (HR2–HR3; the full invariant set lands at HR6 per plan
// §2.8). A verifier is constructed over a frozen `Hir` and run against a
// `DiagnosticReporter`; each structural rule is a private method `verify()` calls
// in turn. HR2 shipped `checkExpressionTypes`; HR3 added `checkNodeArity` and
// `checkBreakContinueScoping`. HR6 grows the class further (Call-arg-vs-FnSig,
// intrinsic-registered, block-structural-termination, shader restrictions) by
// adding sibling rule methods to the same `verify()` body.
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

    // Run every rule, reporting each violation into `reporter`. Returns true
    // iff THIS run emitted no Error-severity diagnostic (computed by delta on the
    // reporter's error count, so a reporter carrying prior-phase errors doesn't
    // make a clean module look dirty).
    [[nodiscard]] bool verify(DiagnosticReporter& reporter) const;

private:
    // Every node whose kind `requiresValidType` (the Expressions group + TypeRef
    // + VarDecl) must carry a `typeId.valid()`. A node flagged `HasError` is
    // skipped (cascade suppression). Each miss emits `H_TypeUnresolved`.
    void checkExpressionTypes(DiagnosticReporter& reporter) const;

    // Every node's child count must satisfy its kind's `childArity` (the single
    // arity source of truth in hir_node.hpp). `ForStmt`/`CaseArm` carry extra
    // payload-coupled constraints checked here. Each violation emits
    // `H_VerifierFailure`.
    void checkNodeArity(DiagnosticReporter& reporter) const;

    // Every `BreakStmt`/`ContinueStmt`'s nesting index must name an enclosing
    // loop/switch (`enclosingBranchTargets`); a `ContinueStmt`'s resolved target
    // must be a loop, not a switch. Each violation emits `H_InvalidBreak`.
    void checkBreakContinueScoping(DiagnosticReporter& reporter) const;

    Hir const& hir_;
};

} // namespace dss
