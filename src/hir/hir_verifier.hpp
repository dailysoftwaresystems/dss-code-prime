#pragma once

#include "core/export.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"   // HirSourceMap

namespace dss {

class DiagnosticReporter;

// HIR structural verifier (HR2–HR5; the full invariant set lands at HR6 per plan
// §2.8). A verifier is constructed over a frozen `Hir` and run against a
// `DiagnosticReporter`; each structural rule is a private method `verify()` calls
// in turn. HR2 shipped the type rule (now `checkRequiredTypes`); HR3 added `checkNodeArity` and
// `checkBreakContinueScoping`; HR4 added `checkDeclarationShape`. HR6 grows the
// class further (Call-arg-vs-FnSig, intrinsic-registered, block-structural-
// termination, shader restrictions) by adding sibling rule methods to the same
// `verify()` body.
//
// Discipline (mirrors the semantic analyzer): COLLECT-ALL, never short-circuit —
// every node is checked so one run surfaces every violation. Violations are
// recoverable diagnostics (`reporter.report`), NOT aborts: an untyped expression
// is a diagnosable lowering/analysis outcome, not a compiler-internal invariant
// breach (those still abort at the substrate/builder layer).
//
// Source spans (HR5): an OPTIONAL `HirSourceMap` may be supplied. When a violating
// node has an entry, its diagnostic carries the real (buffer, span); otherwise the
// diagnostic carries an honest "no location" (`InvalidBuffer` + empty span) and
// the node id travels in the message text. The verifier is fully usable without a
// map (e.g. a unit test that builds HIR directly) — the map only enriches where a
// diagnostic points.
class DSS_EXPORT HirVerifier {
public:
    explicit HirVerifier(Hir const& hir, HirSourceMap const* sourceMap = nullptr) noexcept
        : hir_(hir), sourceMap_(sourceMap) {}

    // The verifier stores a reference and must not outlive the module it
    // inspects — forbid binding to a temporary `Hir` outright. Both arities are
    // deleted: the 2-arg form is needed because the defaulted `sourceMap`
    // parameter would otherwise let `HirVerifier{std::move(h), &map}` bind the
    // rvalue to `Hir const&` and dangle.
    HirVerifier(Hir&&)                      = delete;
    HirVerifier(Hir&&, HirSourceMap const*) = delete;

    // Run every rule, reporting each violation into `reporter`. Returns true
    // iff THIS run emitted no Error-severity diagnostic (computed by delta on the
    // reporter's error count, so a reporter carrying prior-phase errors doesn't
    // make a clean module look dirty).
    [[nodiscard]] bool verify(DiagnosticReporter& reporter) const;

private:
    // Every node whose kind `requiresValidType` (the Expressions group + TypeRef
    // + the source-defined declarations VarDecl/Function/Global/TypeDecl) must
    // carry a `typeId.valid()`. A node flagged `HasError` is skipped (cascade
    // suppression). Each miss emits `H_TypeUnresolved`.
    void checkRequiredTypes(DiagnosticReporter& reporter) const;

    // Every node's child count must satisfy its kind's `childArity` (the single
    // arity source of truth in hir_node.hpp). `ForStmt`/`CaseArm` carry extra
    // payload-coupled constraints checked here. Each violation emits
    // `H_VerifierFailure`.
    void checkNodeArity(DiagnosticReporter& reporter) const;

    // Every `BreakStmt`/`ContinueStmt`'s nesting index must name an enclosing
    // loop/switch (`enclosingBranchTargets`); a `ContinueStmt`'s resolved target
    // must be a loop, not a switch. Each violation emits `H_InvalidBreak`.
    void checkBreakContinueScoping(DiagnosticReporter& reporter) const;

    // Declaration structure (HR4): a `Function`'s last child is its body `Block`
    // and its other children are parameter `VarDecl`s with no initializer; an
    // `ExternFunction` has no body `Block` and only parameter `VarDecl`s. Each
    // violation emits `H_VerifierFailure`.
    void checkDeclarationShape(DiagnosticReporter& reporter) const;

    Hir const&          hir_;
    HirSourceMap const* sourceMap_;   // optional; nullptr = no source provenance
};

} // namespace dss
