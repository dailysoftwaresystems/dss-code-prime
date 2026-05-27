#include "hir/hir_verifier.hpp"

#include "analysis/semantic/type_rules.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/hir_node.hpp"
#include "hir/hir_intrinsic_registry.hpp"

#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace dss {

namespace {

// Emit one HIR verifier diagnostic, locating it via the source map when one is
// available. A node WITH a `HirSourceLoc` gets its real (buffer, span); a node
// without one — a synthetic lowering node with no origin, or any run with no map
// (e.g. a unit test that builds HIR directly) — gets an honest "no location"
// (`InvalidBuffer` + empty span). Either way the node's identity travels in
// `actual` ("hir node #N …"); because `actual` participates in the reporter's
// dedup key, two findings on DIFFERENT nodes are never coalesced even when both
// lack a span and so share the empty one.
void reportAt(DiagnosticReporter& reporter, DiagnosticCode code, HirNodeId id,
              std::string actual, HirSourceMap const* sourceMap) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
    if (sourceMap != nullptr && sourceMap->has(id)) {
        HirSourceLoc const& loc = sourceMap->get(id);
        d.buffer = loc.buffer;
        d.span   = loc.span;
    } else {
        d.buffer = InvalidBuffer;
        d.span   = SourceSpan::empty(0);
    }
    d.actual = std::move(actual);
    reporter.report(std::move(d));
}

} // namespace

bool HirVerifier::verify(DiagnosticReporter& reporter) const {
    std::size_t const errorsBefore = reporter.errorCount();
    checkRequiredTypes(reporter);
    checkNodeArity(reporter);
    checkBreakContinueScoping(reporter);
    checkDeclarationShape(reporter);
    // HR6 rules.
    checkBlockTermination(reporter);
    checkReturnCompleteness(reporter);
    checkCallArguments(reporter);
    checkIntrinsicCalls(reporter);
    checkShaderRestrictions(reporter);
    //
    // A capped reporter (the global maxDiagnostics ceiling hit — here or in a
    // prior phase sharing this reporter) silently drops further report() calls,
    // so the error-count delta below can no longer prove "no violation". Refuse
    // to certify a clean module when the reporter can't have recorded
    // everything — never hand back a false all-clear.
    if (reporter.hitCap()) return false;
    return reporter.errorCount() == errorsBefore;
}

void HirVerifier::checkRequiredTypes(DiagnosticReporter& reporter) const {
    // Linear sweep over every minted node (slot 0 is the sentinel). The node id's
    // arena tag is this module's id, so each `hir_` accessor validates provenance.
    std::uint32_t const moduleTag = hir_.id().v;
    for (std::uint32_t i = 1; i < hir_.nodeCount(); ++i) {
        HirNodeId const id{i, moduleTag};
        HirKind const kind = hir_.kind(id);

        if (!requiresValidType(kind)) continue;
        // Cascade suppression: a node already on a broken path carries HasError;
        // its missing type is a downstream effect, not a new fault to report.
        if (hasError(hir_.flags(id))) continue;
        if (hir_.typeId(id).valid()) continue;

        reportAt(reporter, DiagnosticCode::H_TypeUnresolved, id,
                 std::format("hir node #{} (HirKind ordinal {})",
                             id.v, static_cast<unsigned>(kind)),
                 sourceMap_);
    }
}

void HirVerifier::checkNodeArity(DiagnosticReporter& reporter) const {
    std::uint32_t const moduleTag = hir_.id().v;
    for (std::uint32_t i = 1; i < hir_.nodeCount(); ++i) {
        HirNodeId const id{i, moduleTag};
        HirKind const kind = hir_.kind(id);
        if (hasError(hir_.flags(id))) continue;  // don't pile onto a broken subtree

        auto const count = static_cast<std::uint32_t>(hir_.children(id).size());

        // ForStmt: the child count must equal (present clauses + body), and the
        // payload must not carry bits outside the clause mask.
        if (kind == HirKind::ForStmt) {
            std::uint32_t const p = hir_.payload(id);
            if ((p & ~kForClauseMask) != 0) {
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                         std::format("ForStmt #{} payload {:#x} sets bits outside the "
                                     "clause mask", id.v, p),
                         sourceMap_);
                continue;
            }
            std::uint32_t const expected =
                clauseCount(static_cast<ForClause>(p)) + 1;  // +1 for the body
            if (count != expected) {
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                         std::format("ForStmt #{} has {} children but its clause mask "
                                     "implies {}", id.v, count, expected),
                         sourceMap_);
            }
            continue;
        }
        // CaseArm: a valued (non-default) arm must carry its match-value child.
        if (kind == HirKind::CaseArm) {
            if (!hir_.caseArmIsDefault(id) && count < 1) {
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                         std::format("valued CaseArm #{} has no match-value child", id.v),
                         sourceMap_);
            }
            continue;
        }

        ChildArity const a = childArity(kind);
        bool const ok = count >= a.min && (a.max == kUnboundedArity || count <= a.max);
        if (!ok) {
            std::string const bound = (a.max == kUnboundedArity)
                ? std::format("at least {}", a.min)
                : std::format("[{}, {}]", a.min, a.max);
            reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                     std::format("HirKind ordinal {} node #{} expects {} children, got {}",
                                 static_cast<unsigned>(kind), id.v, bound, count),
                     sourceMap_);
        }
    }
}

void HirVerifier::checkBreakContinueScoping(DiagnosticReporter& reporter) const {
    std::uint32_t const moduleTag = hir_.id().v;
    for (std::uint32_t i = 1; i < hir_.nodeCount(); ++i) {
        HirNodeId const id{i, moduleTag};
        HirKind const kind = hir_.kind(id);
        if (kind != HirKind::BreakStmt && kind != HirKind::ContinueStmt) continue;
        if (hasError(hir_.flags(id))) continue;

        std::uint32_t const depth = hir_.branchDepth(id);
        std::vector<HirNodeId> const targets = enclosingBranchTargets(hir_, id);
        char const* const what = (kind == HirKind::BreakStmt) ? "break" : "continue";

        if (depth >= targets.size()) {
            reportAt(reporter, DiagnosticCode::H_InvalidBreak, id,
                     std::format("{} #{} index {} has no enclosing loop/switch at that "
                                 "depth ({} enclosing)", what, id.v, depth, targets.size()),
                     sourceMap_);
            continue;
        }
        // continue can only target a loop — never a switch.
        if (kind == HirKind::ContinueStmt
            && hir_.kind(targets[depth]) == HirKind::SwitchStmt) {
            reportAt(reporter, DiagnosticCode::H_InvalidBreak, id,
                     std::format("continue #{} index {} resolves to a switch; continue "
                                 "can only target a loop", id.v, depth),
                     sourceMap_);
        }
    }
}

void HirVerifier::checkDeclarationShape(DiagnosticReporter& reporter) const {
    // A parameter is a VarDecl with no initializer (no children). Shared by the
    // Function and ExternFunction checks below.
    auto checkParam = [&](HirNodeId param) {
        if (hir_.kind(param) != HirKind::VarDecl) {
            reportAt(reporter, DiagnosticCode::H_VerifierFailure, param,
                     std::format("parameter #{} must be a VarDecl (HirKind ordinal {})",
                                 param.v, static_cast<unsigned>(hir_.kind(param))),
                     sourceMap_);
        } else if (!hir_.children(param).empty()) {
            reportAt(reporter, DiagnosticCode::H_VerifierFailure, param,
                     std::format("parameter VarDecl #{} must not have an initializer", param.v),
                     sourceMap_);
        }
    };

    std::uint32_t const moduleTag = hir_.id().v;
    for (std::uint32_t i = 1; i < hir_.nodeCount(); ++i) {
        HirNodeId const id{i, moduleTag};
        HirKind const kind = hir_.kind(id);
        if (hasError(hir_.flags(id))) continue;

        if (kind == HirKind::Function) {
            auto kids = hir_.children(id);
            if (kids.empty()) continue;  // arity rule already flagged the missing body
            // Last child is the body Block; the rest are parameters.
            if (hir_.kind(kids.back()) != HirKind::Block) {
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                         std::format("Function #{} body (last child) must be a Block "
                                     "(HirKind ordinal {})",
                                     id.v, static_cast<unsigned>(hir_.kind(kids.back()))),
                         sourceMap_);
            }
            for (HirNodeId param : kids.subspan(0, kids.size() - 1)) checkParam(param);
        } else if (kind == HirKind::ExternFunction) {
            // No body: every child is a parameter, and none may be a Block.
            for (HirNodeId child : hir_.children(id)) {
                if (hir_.kind(child) == HirKind::Block) {
                    reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                             std::format("ExternFunction #{} must not have a body Block "
                                         "(child #{})", id.v, child.v),
                             sourceMap_);
                } else {
                    checkParam(child);
                }
            }
        }
    }
}

// ── HR6 rule helpers ─────────────────────────────────────────────────────────

namespace {

// A statement kind that unconditionally transfers control away, so any sibling
// after it in the same Block is unreachable. The plan §2.8 names Return /
// Unreachable; Break / Continue transfer just as unconditionally, so code after
// them in the same block is equally dead.
bool isUnconditionalTerminator(HirKind k) noexcept {
    return k == HirKind::ReturnStmt || k == HirKind::Unreachable
        || k == HirKind::BreakStmt  || k == HirKind::ContinueStmt;
}

// Does control leave `id` on EVERY structural path by returning or diverging?
// Used by the non-void return-completeness rule. Conservative: a loop does NOT
// count (lowering appends an `Unreachable` after a provably-infinite loop), and
// Break/Continue transfer within a loop rather than out of the function.
bool pathTerminates(Hir const& hir, HirNodeId id) {
    switch (hir.kind(id)) {
        case HirKind::ReturnStmt:
        case HirKind::Unreachable:
            return true;
        case HirKind::Block: {
            auto kids = hir.children(id);
            return !kids.empty() && pathTerminates(hir, kids.back());
        }
        case HirKind::IfStmt: {
            // Terminates iff there is an else AND both branches terminate.
            auto elseB = hir.ifElse(id);
            return elseB.has_value()
                && pathTerminates(hir, hir.ifThen(id))
                && pathTerminates(hir, *elseB);
        }
        case HirKind::SwitchStmt: {
            // Terminates iff there is a default arm AND every arm's body does. A
            // non-CaseArm child is structurally invalid (checkNodeArity reports it
            // but doesn't flag HasError) — treat it conservatively as
            // non-terminating rather than calling a CaseArm-only accessor on it.
            bool hasDefault = false;
            for (HirNodeId arm : hir.switchArms(id)) {
                if (hir.kind(arm) != HirKind::CaseArm) return false;
                auto body = hir.caseArmBody(arm);
                if (body.empty() || !pathTerminates(hir, body.back())) return false;
                if (hir.caseArmIsDefault(arm)) hasDefault = true;
            }
            return hasDefault;
        }
        default:
            return false;
    }
}

// Map each declared function's SymbolId.v to its declaring node (Function or
// ExternFunction). The intra-module call graph keys off this.
std::unordered_map<std::uint32_t, HirNodeId> functionsBySymbol(Hir const& hir) {
    std::unordered_map<std::uint32_t, HirNodeId> map;
    std::uint32_t const tag = hir.id().v;
    for (std::uint32_t i = 1; i < hir.nodeCount(); ++i) {
        HirNodeId const id{i, tag};
        switch (hir.kind(id)) {
            case HirKind::Function:       map.emplace(hir.functionSymbol(id).v, id); break;
            case HirKind::ExternFunction: map.emplace(hir.externFunctionSymbol(id).v, id); break;
            default: break;
        }
    }
    return map;
}

// Pre-order visit of every node in `root`'s subtree (HIR is a tree, so this
// terminates without a visited-set).
template <class F>
void forEachInSubtree(Hir const& hir, HirNodeId root, F const& fn) {
    fn(root);
    for (HirNodeId c : hir.children(root)) forEachInSubtree(hir, c, fn);
}

// The SymbolId.v a Call's callee resolves to when it is a direct `Ref` to a
// symbol; nullopt for an indirect callee (a `Deref`, `Index`, fn-ptr expression…).
std::optional<std::uint32_t> directCalleeSymbol(Hir const& hir, HirNodeId callNode) {
    auto kids = hir.children(callNode);
    if (kids.empty()) return std::nullopt;                 // arity rule flags this
    if (hir.kind(kids.front()) != HirKind::Ref) return std::nullopt;
    return hir.payload(kids.front());
}

} // namespace

void HirVerifier::checkBlockTermination(DiagnosticReporter& reporter) const {
    std::uint32_t const moduleTag = hir_.id().v;
    for (std::uint32_t i = 1; i < hir_.nodeCount(); ++i) {
        HirNodeId const id{i, moduleTag};
        if (hir_.kind(id) != HirKind::Block) continue;
        if (hasError(hir_.flags(id))) continue;

        auto kids = hir_.children(id);
        // An unconditional terminator must be the LAST statement; anything after
        // it is unreachable. One diagnostic per block — the rest is cascade.
        for (std::size_t k = 0; k + 1 < kids.size(); ++k) {
            if (isUnconditionalTerminator(hir_.kind(kids[k]))) {
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, kids[k + 1],
                         std::format("statement #{} in block #{} is unreachable: it follows "
                                     "unconditional terminator #{}",
                                     kids[k + 1].v, id.v, kids[k].v),
                         sourceMap_);
                break;
            }
        }
    }
}

void HirVerifier::checkReturnCompleteness(DiagnosticReporter& reporter) const {
    if (interner_ == nullptr) return;  // the return type can't be read without it
    std::uint32_t const moduleTag = hir_.id().v;
    for (std::uint32_t i = 1; i < hir_.nodeCount(); ++i) {
        HirNodeId const id{i, moduleTag};
        if (hir_.kind(id) != HirKind::Function) continue;
        if (hasError(hir_.flags(id))) continue;

        TypeId const sig = hir_.functionSignature(id);
        if (!sig.valid()) continue;                             // checkRequiredTypes flags this
        if (interner_->kind(sig) != TypeKind::FnSig) continue;  // not a FnSig — caller's contract
        if (interner_->kind(interner_->fnResult(sig)) == TypeKind::Void) continue;  // void: ok

        auto kids = hir_.children(id);
        // A missing/non-Block body is checkDeclarationShape's to report.
        if (kids.empty() || hir_.kind(kids.back()) != HirKind::Block) continue;
        if (!pathTerminates(hir_, kids.back())) {
            reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                     std::format("non-void function #{} may fall through without returning "
                                 "a value", id.v),
                     sourceMap_);
        }
    }
}

void HirVerifier::checkCallArguments(DiagnosticReporter& reporter) const {
    if (interner_ == nullptr) return;  // the FnSig can't be decoded without it
    std::uint32_t const moduleTag = hir_.id().v;
    for (std::uint32_t i = 1; i < hir_.nodeCount(); ++i) {
        HirNodeId const id{i, moduleTag};
        if (hir_.kind(id) != HirKind::Call) continue;
        if (hasError(hir_.flags(id))) continue;

        auto kids = hir_.children(id);
        if (kids.empty()) continue;                       // arity rule flags a callee-less Call
        TypeId const calleeType = hir_.typeId(kids.front());
        if (!calleeType.valid()) continue;                // cascade suppression

        // A function Ref carries the FnSig directly; a function-pointer expression
        // carries `FnPtr<FnSig>` — dereference one level.
        TypeId sig = calleeType;
        if (interner_->kind(calleeType) == TypeKind::FnPtr) {
            auto ops = interner_->operands(calleeType);
            if (ops.empty()) continue;
            sig = ops.front();
        }
        if (interner_->kind(sig) != TypeKind::FnSig) continue;  // opaque/extension callee

        auto params = interner_->fnParams(sig);
        auto args   = kids.subspan(1);
        if (args.size() != params.size()) {
            reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                     std::format("Call #{} passes {} argument(s) but the callee FnSig "
                                 "declares {} parameter(s)", id.v, args.size(), params.size()),
                     sourceMap_);
            continue;  // positions no longer correspond
        }
        for (std::size_t a = 0; a < args.size(); ++a) {
            if (!isAssignable(*interner_, params[a], hir_.typeId(args[a]))) {
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, args[a],
                         std::format("Call #{} argument #{} (node #{}) type is not "
                                     "assignable to the callee's parameter #{}",
                                     id.v, a, args[a].v, a),
                         sourceMap_);
            }
        }
    }
}

void HirVerifier::checkIntrinsicCalls(DiagnosticReporter& reporter) const {
    std::uint32_t const moduleTag = hir_.id().v;
    for (std::uint32_t i = 1; i < hir_.nodeCount(); ++i) {
        HirNodeId const id{i, moduleTag};
        if (hir_.kind(id) != HirKind::IntrinsicCall) continue;
        if (hasError(hir_.flags(id))) continue;

        HirIntrinsicId const intr{hir_.payload(id)};
        if (!hir_.intrinsicRegistry().contains(intr)) {
            reportAt(reporter, DiagnosticCode::H_UnknownIntrinsic, id,
                     std::format("IntrinsicCall #{} references intrinsic id {} which this "
                                 "module's registry never minted", id.v, intr.v),
                     sourceMap_);
        }
    }
}

void HirVerifier::checkShaderRestrictions(DiagnosticReporter& reporter) const {
    std::uint32_t const moduleTag = hir_.id().v;

    // Collect ShaderUsable functions. The common case (no v1 language emits shader
    // nodes yet) is zero — bail before building any graph.
    std::vector<HirNodeId> shaderFns;
    for (std::uint32_t i = 1; i < hir_.nodeCount(); ++i) {
        HirNodeId const id{i, moduleTag};
        if (hir_.kind(id) == HirKind::Function
            && has(hir_.flags(id), HirFlags::ShaderUsable)
            && !hasError(hir_.flags(id))) {
            shaderFns.push_back(id);
        }
    }
    if (shaderFns.empty()) return;

    auto funcs = functionsBySymbol(hir_);

    // Per-call structural checks over each shader function's body subtree.
    for (HirNodeId fn : shaderFns) {
        auto kids = hir_.children(fn);
        if (kids.empty() || hir_.kind(kids.back()) != HirKind::Block) continue;
        forEachInSubtree(hir_, kids.back(), [&](HirNodeId n) {
            if (hir_.kind(n) != HirKind::Call) return;
            auto sym = directCalleeSymbol(hir_, n);
            if (!sym.has_value()) {
                reportAt(reporter, DiagnosticCode::H_ShaderViolation, n,
                         std::format("indirect / function-pointer call #{} is not allowed in "
                                     "a shader function", n.v),
                         sourceMap_);
                return;
            }
            // A direct Ref to a function-pointer variable is also indirect — only
            // the interner can see the FnPtr type.
            if (interner_ != nullptr) {
                TypeId const ct = hir_.typeId(hir_.children(n).front());
                if (ct.valid() && interner_->kind(ct) == TypeKind::FnPtr) {
                    reportAt(reporter, DiagnosticCode::H_ShaderViolation, n,
                             std::format("function-pointer call #{} is not allowed in a "
                                         "shader function", n.v),
                             sourceMap_);
                    return;
                }
            }
            if (auto it = funcs.find(*sym);
                it != funcs.end() && !has(hir_.flags(it->second), HirFlags::ShaderUsable)) {
                reportAt(reporter, DiagnosticCode::H_ShaderViolation, n,
                         std::format("shader function calls non-shader (host) function "
                                     "(call #{})", n.v),
                         sourceMap_);
            }
        });
    }

    // Recursion: any call-graph cycle reachable from a shader function. Build the
    // intra-module direct call graph (function symbol -> resolvable callee symbols).
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> graph;
    for (auto const& [sym, fnNode] : funcs) {
        if (hir_.kind(fnNode) != HirKind::Function) continue;  // externs are leaves
        auto kids = hir_.children(fnNode);
        if (kids.empty() || hir_.kind(kids.back()) != HirKind::Block) continue;
        auto& edges = graph[sym];
        forEachInSubtree(hir_, kids.back(), [&](HirNodeId n) {
            if (hir_.kind(n) != HirKind::Call) return;
            if (auto s = directCalleeSymbol(hir_, n); s && funcs.contains(*s))
                edges.push_back(*s);
        });
    }

    // Grey/black DFS: a back-edge to a node on the current stack is a cycle.
    std::unordered_map<std::uint32_t, int> color;  // 0 unseen, 1 on-stack, 2 done
    std::function<bool(std::uint32_t)> reachesCycle = [&](std::uint32_t s) -> bool {
        color[s] = 1;
        if (auto it = graph.find(s); it != graph.end()) {
            for (std::uint32_t t : it->second) {
                if (color[t] == 1) return true;
                if (color[t] == 0 && reachesCycle(t)) return true;
            }
        }
        color[s] = 2;
        return false;
    };
    for (HirNodeId fn : shaderFns) {
        color.clear();
        if (reachesCycle(hir_.functionSymbol(fn).v)) {
            reportAt(reporter, DiagnosticCode::H_ShaderViolation, fn,
                     std::format("shader function #{} reaches a recursive call cycle "
                                 "(recursion is not allowed in shaders)", fn.v),
                     sourceMap_);
        }
    }
}

} // namespace dss
