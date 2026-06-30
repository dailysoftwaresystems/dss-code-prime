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
// Structural-termination predicate. Returns true iff control leaves
// `id` on EVERY structural path by returning or diverging
// (`ReturnStmt`, `Unreachable`, or a nested Block/If/Switch whose
// every reachable arm terminates). Conservative — a loop's body is
// NOT counted as terminating (lowering appends `Unreachable` after a
// provably-infinite loop to make the structural answer match the
// dynamic one).
//
// Exposed at the verifier-substrate header so other HIR-tier
// transformations (CST-to-HIR's main-implicit-return-0 insertion
// for D-LK10-ENTRY-MAIN-IMPLICIT-RETURN; future const-eval +
// reachability passes) can share the SINGLE source of truth for
// "this construct terminates control" — duplicating the recursion
// elsewhere would silently drift if a new HIR kind extends the
// terminator set (e.g. ThrowStmt for a hypothetical exception
// model).
//
// Templated over the source so it works against EITHER a frozen
// `Hir` (the verifier's consumer) or a mid-build `HirBuilder` (the
// CST-to-HIR lowering's consumer). Required source interface:
// `kind(id) -> HirKind`, `children(id) -> span`, `ifThen(id)`,
// `ifElse(id) -> optional`, `switchArms(id) -> span`,
// `switchBody(id) -> HirNodeId`, `caseArmIsDefault(id) -> bool`. Both
// `Hir` and `HirBuilder` satisfy this interface.
template <typename Source>
[[nodiscard]] bool pathTerminates(Source const& src, HirNodeId id) {
    switch (src.kind(id)) {
        case HirKind::ReturnStmt:
        case HirKind::Unreachable:
            return true;
        // FC5: `goto` unconditionally transfers control — it never falls through,
        // so a block ending in `goto` definitely leaves. This is sound for the
        // fall-off-end invariant: if the goto's target leads nowhere, THAT block's
        // own structural check catches the fall-off (matches the break/continue +
        // infinite-loop-as-Unreachable treatment). A LabelStmt is transparent —
        // it terminates iff its labeled statement does (so `end: return 0;` as a
        // function's last statement still terminates).
        case HirKind::GotoStmt:
        // D-CSUBSET-COMPUTED-GOTO: `goto *expr;` also transfers unconditionally —
        // it never falls through (same fall-off-end soundness as plain goto).
        case HirKind::IndirectGotoStmt:
            return true;
        case HirKind::Block:
        case HirKind::LabelStmt: {
            auto kids = src.children(id);
            return !kids.empty() && pathTerminates(src, kids.back());
        }
        case HirKind::IfStmt: {
            auto elseB = src.ifElse(id);
            return elseB.has_value()
                && pathTerminates(src, src.ifThen(id))
                && pathTerminates(src, *elseB);
        }
        case HirKind::SwitchStmt: {
            // c60 (Design I-A): a switch terminates on all paths iff (a) it has a
            // `default:` arm — so the discriminant always lands inside the body
            // rather than skipping past to the join — AND (b) the flat body Block
            // terminates (its last statement, through transparent case markers,
            // ends in a Return/Unreachable/goto and nothing falls off the end).
            // Fall-through is straight-line in the flat body, so the per-arm
            // termination the old grouped shape checked reduces to the body Block's
            // own termination (MORE accurate: `case A: foo(); default: return;` now
            // correctly terminates via fall-through, where the grouped form did not).
            bool hasDefault = false;
            for (HirNodeId arm : src.switchArms(id)) {
                if (src.kind(arm) != HirKind::CaseArm) return false;
                if (src.caseArmIsDefault(arm)) hasDefault = true;
            }
            return hasDefault && pathTerminates(src, src.switchBody(id));
        }
        default:
            return false;
    }
}

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
    // extended to all unconditional transfers): a statement following an
    // unconditional terminator (`Return`/`Unreachable`/`Break`/`Continue`) within
    // a `Block` can never be reached. ISO C permits this (C 6.8.x imposes no
    // reachability constraint), so — matching real compilers — each occurrence is
    // a WARNING (`H_UnreachableCode`), NOT a rejection: the module still compiles
    // and the dead statement is dropped at the MIR unreachable-prune. One
    // diagnostic per block (the rest is cascade). This is the ONLY verifier rule
    // that does not reject; every other rule emits an Error.
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

    // D5.4-FU1 + FU4: every ConstructAggregate's child count and per-child
    // types must match its declared result type's shape — Struct = N
    // children, one per field, each child's type must equal the
    // corresponding field's type; Union = exactly 1 child whose type
    // matches one of the variant types; Array = `length` children, each
    // typed as the element type. Catches HIR lowering bugs that would
    // otherwise silently produce mis-shaped aggregates downstream.
    void checkConstructAggregate(DiagnosticReporter& reporter) const;

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
