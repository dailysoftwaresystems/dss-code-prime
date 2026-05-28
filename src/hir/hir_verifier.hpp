#pragma once

#include "core/export.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"   // HirSourceMap

namespace dss {

class DiagnosticReporter;
class TypeInterner;

// HIR structural verifier (HR2–HR6; the full invariant set per plan §2.8). A
// verifier is constructed over a frozen `Hir` and run against a
// `DiagnosticReporter`; each structural rule is a private method `verify()` calls
// in turn. HR2 shipped the type rule (now `checkRequiredTypes`); HR3 added `checkNodeArity` and
// `checkBreakContinueScoping`; HR4 added `checkDeclarationShape`; HR6 added block
// termination, return completeness, Call-arg-vs-FnSig, intrinsic-registered, and
// the shader-restriction subverifier.
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
//
// Type interner (HR6): an OPTIONAL `TypeInterner` may be supplied. The rules that
// must decode a `TypeId` — Call argument-vs-FnSig matching, non-void return
// completeness, and function-pointer detection in shaders — RUN ONLY when it is
// present; without it they are skipped (a module built directly in a test, with
// no semantic phase, has no interner to consult). The real pipeline always
// supplies the interner the semantic phase produced, so these rules always run
// in production.
class DSS_EXPORT HirVerifier {
public:
    explicit HirVerifier(Hir const& hir, HirSourceMap const* sourceMap = nullptr,
                         TypeInterner const* interner = nullptr) noexcept
        : hir_(hir), sourceMap_(sourceMap), interner_(interner) {}

    // The verifier stores a reference and must not outlive the module it
    // inspects — forbid binding to a temporary `Hir` outright. Every arity is
    // deleted: each defaulted parameter would otherwise let an
    // `HirVerifier{std::move(h), ...}` bind the rvalue to `Hir const&` and dangle.
    HirVerifier(Hir&&)                                           = delete;
    HirVerifier(Hir&&, HirSourceMap const*)                      = delete;
    HirVerifier(Hir&&, HirSourceMap const*, TypeInterner const*) = delete;

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

    // Block dead-code (HR6, plan §2.8 "no fall-through past Return/Unreachable",
    // extended to all unconditional transfers): no statement may follow an
    // unconditional terminator (`Return`/`Unreachable`/`Break`/`Continue`) within
    // a `Block` — control can never reach it. Each violation emits
    // `H_VerifierFailure`.
    void checkBlockTermination(DiagnosticReporter& reporter) const;

    // Return completeness (HR6, plan §2.8): a non-void `Function`'s body `Block`
    // must terminate on every structural path (each path ends in a `Return` or
    // `Unreachable`). Loops are conservatively non-terminating, so lowering must
    // append an `Unreachable` after a provably-infinite loop. Interner-gated (the
    // return type is read from the FnSig). Each violation emits `H_VerifierFailure`.
    void checkReturnCompleteness(DiagnosticReporter& reporter) const;

    // Call arguments (HR6, plan §2.8): a `Call`'s argument count and types must
    // match the callee's `FnSig` (each arg `isAssignable` to its parameter).
    // Interner-gated (the FnSig is decoded from the callee's `TypeId`). Each
    // violation emits `H_VerifierFailure`.
    void checkCallArguments(DiagnosticReporter& reporter) const;

    // Intrinsics (HR6, plan §2.8): every `IntrinsicCall`'s payload must resolve to
    // an intrinsic the module's `HirIntrinsicRegistry` minted. Each miss emits
    // `H_UnknownIntrinsic`.
    void checkIntrinsicCalls(DiagnosticReporter& reporter) const;

    // D5.1: every `MemberAccess`'s `payload` (the field index) must be in
    // bounds for the base's struct/union type. Interner-gated (the field
    // count is decoded from the base's TypeId). Each violation emits
    // `H_VerifierFailure`. Catches HIR-lowering bugs that produced a stale
    // or off-by-one field index — the front-end's SymbolRecord::fieldIndex
    // is the source of truth at lowering time, but the verifier re-checks
    // here so any direct-builder construction path (tests, synthetic IR)
    // is covered too.
    void checkMemberAccess(DiagnosticReporter& reporter) const;

    // Shader restrictions (HR6, plan §2.8): inside a `ShaderUsable` function's
    // subtree — no recursion (call-graph cycle), no indirect / function-pointer
    // call, no call to a non-shader (host) function. Each violation emits
    // `H_ShaderViolation`. (Dynamic allocation is not yet expressible in HIR.)
    void checkShaderRestrictions(DiagnosticReporter& reporter) const;

    Hir const&          hir_;
    HirSourceMap const* sourceMap_;   // optional; nullptr = no source provenance
    TypeInterner const* interner_;    // optional; nullptr = skip type-decoding rules
};

} // namespace dss
