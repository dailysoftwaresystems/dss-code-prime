#include "hir/hir_verifier.hpp"

#include "analysis/semantic/type_rules.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"   // c115: BuiltinLowering (SEH context)
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
//
// `severity` defaults to `Error` — almost every verifier rule reports a
// structural-invariant breach that REJECTS the module, so every existing call
// site stays unchanged. The one exception is `checkBlockTermination`'s
// unreachable-after-terminator finding, which is ISO-C-valid and so passes
// `DiagnosticSeverity::Warning` (a warning doesn't bump `errorCount`, so
// `verify()` still returns true and the module compiles).
void reportAt(DiagnosticReporter& reporter, DiagnosticCode code, HirNodeId id,
              std::string actual, HirSourceMap const* sourceMap,
              DiagnosticSeverity severity = DiagnosticSeverity::Error) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = severity;
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
    checkSehContext(reporter);
    checkDeclarationShape(reporter);
    // HR6 rules.
    checkBlockTermination(reporter);
    checkReturnCompleteness(reporter);
    checkCallArguments(reporter);
    checkIntrinsicCalls(reporter);
    checkMemberAccess(reporter);
    checkConstructAggregate(reporter);
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

        // SeqExpr: the result (last child) must be a value-yielding expression —
        // it supplies the SeqExpr's type. A statement in result position is
        // malformed (the arity check below separately enforces ≥1 child).
        if (kind == HirKind::SeqExpr && count >= 1) {
            HirNodeId const result = hir_.children(id).back();
            if (!requiresValidType(hir_.kind(result))) {
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                         std::format("SeqExpr #{} result child (last) is not a "
                                     "value-yielding expression", id.v),
                         sourceMap_);
            }
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
        std::vector<HirNodeId> targets = enclosingBranchTargets(hir_, id);
        char const* const what = (kind == HirKind::BreakStmt) ? "break" : "continue";

        // C 6.8.6.2: `continue` targets the innermost ITERATION statement; a switch is
        // TRANSPARENT to continue (it is a `break` target, NOT a `continue` target).
        // Drop switch frames so the depth indexes loops only — a `continue` inside a
        // switch (e.g. sqlite3VXPrintf's format loop) correctly reaches the enclosing
        // loop. `break` keeps the full loops+switches stack (a switch IS a break target).
        if (kind == HirKind::ContinueStmt) {
            std::erase_if(targets, [&](HirNodeId t) {
                return hir_.kind(t) == HirKind::SwitchStmt;
            });
        }

        if (depth >= targets.size()) {
            reportAt(reporter, DiagnosticCode::H_InvalidBreak, id,
                     std::format("{} #{} index {} has no enclosing {} at that "
                                 "depth ({} enclosing)", what, id.v, depth,
                                 kind == HirKind::ContinueStmt ? "loop" : "loop/switch",
                                 targets.size()),
                     sourceMap_);
            continue;
        }
    }
}

// c115 SEH (D-WIN64-SEH-FUNCLETS): the __try/__except context rules. THE
// chokepoint — runs verify-on-load after EVERY frontend lowering (and on
// hir_text loads), so no frontend can bypass it. Four rule families:
//   (1) H_SehBuiltinContext — `_exception_code()` needs an enclosing __except
//       filter-expression OR handler-body; `_exception_info()` a filter only.
//   (2) H_SehJumpIntoRegion — a goto may not enter any part of a __try
//       statement it is not already inside (MSVC rejects it too).
//   (3) H_SehEarlyExit (D-CSUBSET-SEH-EARLY-EXIT, trigger-gated, option (C) of
//       the c115 design-audit) — no return / goto-out / break-out /
//       continue-out / indirect-goto from INSIDE a guarded body: the guarded
//       body keeps exactly ONE exit (the fall-through) so the c116 scope-table
//       region membership stays CFG-derivable. sqlite: zero early exits.
//   (4) H_SehLabelAddress (D-CSUBSET-SEH-LABEL-ADDR, trigger-gated) — `&&label`
//       naming a label inside any part of a __try statement.
void HirVerifier::checkSehContext(DiagnosticReporter& reporter) const {
    std::uint32_t const moduleTag = hir_.id().v;

    // Zero-cost for the non-SEH world: one flat scan for either a SehTryExcept
    // node OR a SEH intrinsic builtin (`_exception_code`/`_exception_info`),
    // then bail. The intrinsic must be included so a BARE `_exception_code()`
    // with no enclosing __try is rejected HERE (H_SehBuiltinContext) rather
    // than flowing to the mir_to_lir funclet fail-loud.
    bool anySeh = false;
    for (std::uint32_t i = 1; i < hir_.nodeCount() && !anySeh; ++i) {
        HirNodeId const id{i, moduleTag};
        HirKind const k = hir_.kind(id);
        if (k == HirKind::SehTryExcept) { anySeh = true; break; }
        if (k == HirKind::BuiltinCall) {
            auto const bl = static_cast<BuiltinLowering>(hir_.payload(id));
            if (bl == BuiltinLowering::SehExceptionCode
                || bl == BuiltinLowering::SehExceptionInfo) anySeh = true;
        }
    }
    if (!anySeh) return;

    // The (sehNode, childSlot) ancestry of `id`, innermost-first: every
    // SehTryExcept ancestor + which of its three children the path enters
    // through (0 = guarded body, 1 = filter expression, 2 = handler body).
    auto const sehAncestry = [&](HirNodeId id) {
        std::vector<std::pair<HirNodeId, unsigned>> out;
        HirNodeId prev = id;
        for (HirNodeId cur = hir_.parent(id); cur.valid(); cur = hir_.parent(cur)) {
            if (hir_.kind(cur) == HirKind::SehTryExcept) {
                auto const kids = hir_.children(cur);
                unsigned slot = 3;   // 3 = not a direct child (defensive)
                for (unsigned s = 0; s < kids.size() && s < 3u; ++s)
                    if (kids[s] == prev) { slot = s; break; }
                out.emplace_back(cur, slot);
            }
            prev = cur;
        }
        return out;
    };
    auto const contains = [](std::vector<std::pair<HirNodeId, unsigned>> const& anc,
                             HirNodeId seh, unsigned slot) {
        for (auto const& [s, sl] : anc)
            if (s == seh && sl == slot) return true;
        return false;
    };

    // Per-function label-ordinal → LabelStmt map (labels are function-scoped;
    // built lazily since only goto/label-addr rules need it).
    auto const enclosingFunction = [&](HirNodeId id) {
        for (HirNodeId cur = hir_.parent(id); cur.valid(); cur = hir_.parent(cur))
            if (hir_.kind(cur) == HirKind::Function) return cur;
        return HirNodeId{};
    };
    std::unordered_map<std::uint64_t, HirNodeId> labelByFnOrd;
    for (std::uint32_t i = 1; i < hir_.nodeCount(); ++i) {
        HirNodeId const id{i, moduleTag};
        if (hir_.kind(id) != HirKind::LabelStmt) continue;
        HirNodeId const fn = enclosingFunction(id);
        if (!fn.valid()) continue;
        std::uint64_t const key =
            (static_cast<std::uint64_t>(fn.v) << 32) | hir_.labelOrdinal(id);
        labelByFnOrd.emplace(key, id);
    }
    auto const resolveLabel = [&](HirNodeId from, std::uint32_t ord) {
        HirNodeId const fn = enclosingFunction(from);
        if (!fn.valid()) return HirNodeId{};
        auto const it = labelByFnOrd.find(
            (static_cast<std::uint64_t>(fn.v) << 32) | ord);
        return it == labelByFnOrd.end() ? HirNodeId{} : it->second;
    };

    for (std::uint32_t i = 1; i < hir_.nodeCount(); ++i) {
        HirNodeId const id{i, moduleTag};
        if (hasError(hir_.flags(id))) continue;
        switch (hir_.kind(id)) {
        case HirKind::BuiltinCall: {
            auto const bl = static_cast<BuiltinLowering>(hir_.payload(id));
            if (bl != BuiltinLowering::SehExceptionCode
                && bl != BuiltinLowering::SehExceptionInfo) break;
            bool const isInfo = (bl == BuiltinLowering::SehExceptionInfo);
            bool legal = false;
            for (auto const& [seh, slot] : sehAncestry(id)) {
                (void)seh;
                if (slot == 1u || (!isInfo && slot == 2u)) { legal = true; break; }
            }
            if (!legal) {
                reportAt(reporter, DiagnosticCode::H_SehBuiltinContext, id,
                         std::format("'{}' is valid only inside an __except {}",
                                     isInfo ? "_exception_info" : "_exception_code",
                                     isInfo ? "filter expression"
                                            : "filter expression or handler body"),
                         sourceMap_);
            }
            break;
        }
        case HirKind::ReturnStmt: {
            for (auto const& [seh, slot] : sehAncestry(id)) {
                (void)seh;
                if (slot == 0u) {
                    reportAt(reporter, DiagnosticCode::H_SehEarlyExit, id,
                             "'return' inside a __try guarded body is not "
                             "supported (D-CSUBSET-SEH-EARLY-EXIT: the guarded "
                             "body keeps a single fall-through exit; "
                             "trigger-gated — no shipped consumer)",
                             sourceMap_);
                    break;
                }
            }
            break;
        }
        case HirKind::GotoStmt: {
            auto const gAnc = sehAncestry(id);
            HirNodeId const label = resolveLabel(id, hir_.labelOrdinal(id));
            if (!label.valid()) break;   // unresolved label — not this rule's job
            auto const lAnc = sehAncestry(label);
            for (auto const& [seh, slot] : lAnc) {
                if (!contains(gAnc, seh, slot)) {
                    reportAt(reporter, DiagnosticCode::H_SehJumpIntoRegion, id,
                             "goto target label is inside a part of a __try "
                             "statement that does not enclose the goto (a jump "
                             "may not enter a guarded region)",
                             sourceMap_);
                    break;
                }
            }
            for (auto const& [seh, slot] : gAnc) {
                if (slot == 0u && !contains(lAnc, seh, 0u)) {
                    reportAt(reporter, DiagnosticCode::H_SehEarlyExit, id,
                             "goto out of a __try guarded body is not supported "
                             "(D-CSUBSET-SEH-EARLY-EXIT: the guarded body keeps "
                             "a single fall-through exit; trigger-gated — no "
                             "shipped consumer)",
                             sourceMap_);
                    break;
                }
            }
            break;
        }
        case HirKind::IndirectGotoStmt: {
            for (auto const& [seh, slot] : sehAncestry(id)) {
                (void)seh;
                if (slot == 0u) {
                    reportAt(reporter, DiagnosticCode::H_SehEarlyExit, id,
                             "computed goto inside a __try guarded body is not "
                             "supported (D-CSUBSET-SEH-EARLY-EXIT: its target "
                             "set cannot be proven region-internal)",
                             sourceMap_);
                    break;
                }
            }
            break;
        }
        case HirKind::LabelAddressOf: {
            HirNodeId const label = resolveLabel(id, hir_.payload(id));
            if (!label.valid()) break;
            if (!sehAncestry(label).empty()) {
                reportAt(reporter, DiagnosticCode::H_SehLabelAddress, id,
                         "'&&label' naming a label inside a __try statement is "
                         "not supported (D-CSUBSET-SEH-LABEL-ADDR: a computed "
                         "goto could enter the guarded range; trigger-gated — "
                         "no shipped consumer)",
                         sourceMap_);
            }
            break;
        }
        case HirKind::BreakStmt:
        case HirKind::ContinueStmt: {
            // The break/continue TARGET (loop/switch node) must not sit outside
            // a guarded body the statement is inside — i.e. walking up from the
            // statement, a (seh, guarded-body) crossing BEFORE the target frame
            // is an early exit. Mirrors checkBreakContinueScoping's target
            // resolution (incl. continue's switch transparency).
            std::uint32_t const depth = hir_.branchDepth(id);
            std::vector<HirNodeId> targets = enclosingBranchTargets(hir_, id);
            if (hir_.kind(id) == HirKind::ContinueStmt) {
                std::erase_if(targets, [&](HirNodeId t) {
                    return hir_.kind(t) == HirKind::SwitchStmt;
                });
            }
            if (depth >= targets.size()) break;   // checkBreakContinueScoping owns it
            HirNodeId const target = targets[depth];
            HirNodeId prev = id;
            for (HirNodeId cur = hir_.parent(id); cur.valid();
                 cur = hir_.parent(cur)) {
                if (cur == target) break;   // reached the frame first — legal
                if (hir_.kind(cur) == HirKind::SehTryExcept) {
                    auto const kids = hir_.children(cur);
                    if (!kids.empty() && kids[0] == prev) {
                        reportAt(reporter, DiagnosticCode::H_SehEarlyExit, id,
                                 std::format(
                                     "'{}' exiting a __try guarded body is not "
                                     "supported (D-CSUBSET-SEH-EARLY-EXIT: the "
                                     "guarded body keeps a single fall-through "
                                     "exit; trigger-gated — no shipped consumer)",
                                     hir_.kind(id) == HirKind::BreakStmt
                                         ? "break" : "continue"),
                                 sourceMap_);
                        break;
                    }
                }
                prev = cur;
            }
            break;
        }
        default: break;
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
// them in the same block is equally dead. FC5: `goto` likewise — code after
// `goto X;` (other than the target LabelStmt itself) is unreachable.
bool isUnconditionalTerminator(HirKind k) noexcept {
    return k == HirKind::ReturnStmt || k == HirKind::Unreachable
        || k == HirKind::BreakStmt  || k == HirKind::ContinueStmt
        || k == HirKind::GotoStmt;
}

// (pathTerminates lives in hir_verifier.hpp as a header template
// over `Source` — both `Hir` and `HirBuilder` satisfy the required
// interface. The check call below resolves to the `Hir` instantiation.)

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
        // it is unreachable. ISO C permits unreachable statements (no reachability
        // constraint in C 6.8.x), so this is a WARNING, not a rejection — the
        // module still compiles and the dead statement is dropped at the MIR
        // unreachable-prune (so runtime is unaffected). One diagnostic per block
        // — the rest is cascade.
        for (std::size_t k = 0; k + 1 < kids.size(); ++k) {
            if (isUnconditionalTerminator(hir_.kind(kids[k]))) {
                // FC5 carve-out: a LabelStmt is a control-flow merge point (a
                // goto/label target), so it is REACHABLE even immediately after an
                // unconditional terminator — `goto X; X: …;` must NOT warn. Skip it
                // and keep scanning so a genuinely-dead non-label statement later
                // (`goto X; foo(); X: …`) still warns.
                if (hir_.kind(kids[k + 1]) == HirKind::LabelStmt) continue;
                reportAt(reporter, DiagnosticCode::H_UnreachableCode, kids[k + 1],
                         std::format("statement #{} in block #{} is unreachable: it follows "
                                     "unconditional terminator #{}",
                                     kids[k + 1].v, id.v, kids[k].v),
                         sourceMap_, DiagnosticSeverity::Warning);
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
        // D-LANG-VARIADIC (step 13.4): a variadic FnSig admits args
        // beyond `fnParams().size()` — the declared params are the
        // FIXED prefix; positions [fixedCount, args.size()) are
        // vararg-region positions whose types are not constrained
        // by the FnSig (C's default-argument-promotion + the
        // platform's vararg ABI handle them at the call site). The
        // arity check rejects only when there are FEWER args than
        // fixed params. For non-variadic FnSigs the check is
        // unchanged (exact match).
        bool const isVariadic = interner_->fnIsVariadic(sig);
        bool const arityBad   = isVariadic
            ? (args.size() < params.size())
            : (args.size() != params.size());
        if (arityBad) {
            reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                     std::format("Call #{} passes {} argument(s) but the callee FnSig "
                                 "declares {} {}parameter(s)", id.v, args.size(),
                                 params.size(), isVariadic ? "fixed " : ""),
                     sourceMap_);
            continue;  // positions no longer correspond
        }
        // D-HIR-VERIFIER-POINTER-CONVERT-CONTRACT (anchor; step 13.2
        // closure, 2026-06-02): the 4th `isAssignable` parameter is
        // omitted here, so it defaults to `PointerConversionRules{}`
        // (both flags false → strict reject). The verifier sees the
        // POST-coerce HIR; all implicit pointer conversions admitted
        // by the active language's `pointerConversions` block were
        // already materialized as explicit `HirKind::Cast` nodes by
        // `cst_to_hir.cpp::coerce()`, so a bare `Ptr<T>→Ptr<Void>`
        // arg pair reaching this check IS a real bug — not a missed
        // implicit conversion. Closure: when the first non-`cst_to_hir`
        // HIR producer arrives (FFI shim, trampoline synthesizer,
        // JIT path), add an audit test that constructs HIR with a
        // bare `Ptr<int>→Ptr<Void>` call arg WITHOUT a Cast and pins
        // `H_VerifierFailure` fires here.
        // D-LANG-VARIADIC (step 13.4): vararg-region args (positions
        // >= params.size() on a variadic FnSig) have no type
        // constraint from the FnSig — the per-arg assignability check
        // bounds at `params.size()`, not `args.size()`. C's default
        // argument promotion + the platform vararg ABI handle vararg
        // typing at the call site, not at the verifier.
        for (std::size_t a = 0; a < params.size(); ++a) {
            if (!isAssignable(*interner_, params[a], hir_.typeId(args[a]))) {
                // D-LANG-NULL-POINTER-CONSTANT verifier fallback
                // (step 13.3 macOS CI fix 2026-06-02): if `cst_to_hir.cpp`'s
                // coerce arm failed to materialize the Cast for a
                // null-pointer-constant IntLit (host-dependent path
                // surfaced via macOS CI but not Windows), the bare
                // `IntLit(0) → Ptr<*>` arrives here and would trip a
                // false-positive H_VerifierFailure. Admit when the
                // structural pattern holds: arg is `HirKind::Literal`
                // of integer kind, param is `Ptr<*>`. Value-0 is
                // guaranteed by the semantic-tier admission
                // (`semantic_analyzer.cpp::admitsNullPointerConstant`
                // → `isLiteralIntegerZero`) — a non-zero literal
                // would have produced `S_TypeMismatch` before HIR
                // lowering, so reaching the verifier with arg=IntLit
                // in a Ptr<*> slot implies value==0. This is the
                // structural-invariant guarantee D-HIR-VERIFIER-
                // POINTER-CONVERT-CONTRACT documents as the
                // post-coerce invariant — extended here to the
                // null-pointer-constant case which is admitted at
                // value-tier (semantic) rather than type-tier
                // (isAssignable).
                // Helper: walk through Cast wrappers to find the
                // effective leaf, returning the leaf node + its
                // resolved type. coerce()'s host-divergent path could
                // either (a) skip Cast emission entirely (args[a]
                // arrives as bare Literal) OR (b) emit a Cast with a
                // mismatched typeId. The fallback walks Cast wrappers
                // to handle both — the structural invariant pins
                // "is this ultimately an integer-typed literal" not
                // "is this directly a Literal node".
                auto findEffectiveIntegerLiteral = [&](HirNodeId start) {
                    struct R { HirKind kind; TypeId ty; };
                    HirNodeId cur = start;
                    // Bounded descent: walk through at most a handful
                    // of Cast wrappers (defensive against unbounded
                    // chains; coerce only ever emits a single Cast).
                    for (int hop = 0; hop < 4 && cur.valid(); ++hop) {
                        HirKind const k  = hir_.kind(cur);
                        TypeId  const ty = hir_.typeId(cur);
                        if (k == HirKind::Literal) {
                            return R{k, ty};
                        }
                        if (k != HirKind::Cast) break;
                        auto const kids = hir_.children(cur);
                        if (kids.empty()) break;
                        cur = kids[0];
                    }
                    return R{HirKind::Module, InvalidType};  // sentinel non-match
                };
                bool nullPtrConstant = false;
                TypeId const paramTy = params[a];
                if (paramTy.valid()
                    && interner_->kind(paramTy) == TypeKind::Ptr) {
                    auto const eff = findEffectiveIntegerLiteral(args[a]);
                    if (eff.kind == HirKind::Literal && eff.ty.valid()) {
                        auto const ck = interner_->kind(eff.ty);
                        if (ck == TypeKind::I8   || ck == TypeKind::I16
                         || ck == TypeKind::I32  || ck == TypeKind::I64
                         || ck == TypeKind::I128
                         || ck == TypeKind::U8   || ck == TypeKind::U16
                         || ck == TypeKind::U32  || ck == TypeKind::U64
                         || ck == TypeKind::U128) {
                            // Semantic gate guarantees value==0 for
                            // any integer literal admitted in a
                            // Ptr<*> slot (a non-zero literal would
                            // have produced S_TypeMismatch before
                            // HIR lowering). Safe to admit
                            // structurally without value-tier
                            // re-check (which is the host-divergent
                            // path we're guarding against).
                            nullPtrConstant = true;
                        }
                    }
                }
                if (nullPtrConstant) continue;
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, args[a],
                         std::format("Call #{} argument #{} (node #{}) type is not "
                                     "assignable to the callee's parameter #{}",
                                     id.v, a, args[a].v, a),
                         sourceMap_);
            }
        }
    }
}

void HirVerifier::checkMemberAccess(DiagnosticReporter& reporter) const {
    // D5.1: every MemberAccess's payload (field index) must be in bounds
    // for the base's struct/union type. Interner-gated -- we decode the
    // field count via `operands(baseType)`. The HIR-lowering reads the
    // field's `SymbolRecord::fieldIndex` (set by Pass 1 to the field's
    // declaration-order ordinal), so this check catches HIR-lowering
    // bugs OR direct-builder synthetic-IR misuse (a hand-fabricated
    // MemberAccess with an off-by-one field index).
    if (interner_ == nullptr) return;
    std::uint32_t const moduleTag = hir_.id().v;
    for (std::uint32_t i = 1; i < hir_.nodeCount(); ++i) {
        HirNodeId const id{i, moduleTag};
        if (hir_.kind(id) != HirKind::MemberAccess) continue;
        if (hasError(hir_.flags(id))) continue;

        auto kids = hir_.children(id);
        if (kids.empty()) continue;                       // arity rule flags this
        TypeId const baseType = hir_.typeId(kids.front());
        if (!baseType.valid()) continue;                   // cascade suppression

        // The base must be a composite (Struct/Union) — for arrow form,
        // the HIR-lowering already inserted a Deref, so the MemberAccess
        // always sees the composite, never a Ptr. If we somehow see a
        // non-composite here, fail loud: the lowering is broken.
        TypeKind const bk = interner_->kind(baseType);
        if (bk != TypeKind::Struct && bk != TypeKind::Union) {
            reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                     std::format("MemberAccess #{} base type is not a struct "
                                 "or union (TypeKind ordinal {})",
                                 id.v, static_cast<unsigned>(bk)),
                     sourceMap_);
            continue;
        }
        auto const fields = interner_->operands(baseType);
        std::uint32_t const fieldIndex = hir_.payload(id);
        if (fieldIndex >= fields.size()) {
            reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                     std::format("MemberAccess #{} field index {} is out of "
                                 "bounds for the base type's {} field(s)",
                                 id.v, fieldIndex, fields.size()),
                     sourceMap_);
        }
    }
}

void HirVerifier::checkConstructAggregate(DiagnosticReporter& reporter) const {
    // D5.4-FU1 + FU4: every ConstructAggregate's child count and types
    // must match its declared result type's shape. The HIR lowering
    // (`lowerBraceInit` / `lowerUnionBraceInit` / `synthZeroOrError`)
    // produces well-formed aggregates by construction; this rule
    // catches lowering BUGS and synthetic-IR misuse.
    if (interner_ == nullptr) return;
    std::uint32_t const moduleTag = hir_.id().v;
    for (std::uint32_t i = 1; i < hir_.nodeCount(); ++i) {
        HirNodeId const id{i, moduleTag};
        if (hir_.kind(id) != HirKind::ConstructAggregate) continue;
        if (hasError(hir_.flags(id))) continue;
        TypeId const aggTy = hir_.typeId(id);
        if (!aggTy.valid()) continue;   // requiresValidType already flagged
        TypeKind const kind = interner_->kind(aggTy);
        auto kids = hir_.children(id);

        if (kind == TypeKind::Struct) {
            auto const fields = interner_->operands(aggTy);
            if (kids.size() != fields.size()) {
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                         std::format("ConstructAggregate #{} (Struct): "
                                     "expected {} children, got {}",
                                     id.v, fields.size(), kids.size()),
                         sourceMap_);
                continue;
            }
            for (std::size_t k = 0; k < kids.size(); ++k) {
                TypeId const childTy = hir_.typeId(kids[k]);
                if (!childTy.valid()) continue;   // requiresValidType flagged
                if (childTy.v != fields[k].v) {
                    reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                             std::format("ConstructAggregate #{} (Struct) "
                                         "child[{}] type {} doesn't match "
                                         "field type {}",
                                         id.v, k, childTy.v, fields[k].v),
                             sourceMap_);
                }
            }
        } else if (kind == TypeKind::Union) {
            if (kids.size() != 1u) {
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                         std::format("ConstructAggregate #{} (Union): "
                                     "expected exactly 1 child (the active "
                                     "variant), got {}",
                                     id.v, kids.size()),
                         sourceMap_);
                continue;
            }
            TypeId const childTy = hir_.typeId(kids[0]);
            if (!childTy.valid()) continue;
            auto const variants = interner_->operands(aggTy);
            bool ok = false;
            for (TypeId vty : variants) {
                if (vty.v == childTy.v) { ok = true; break; }
            }
            if (!ok) {
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                         std::format("ConstructAggregate #{} (Union) child "
                                     "type {} doesn't match any of the "
                                     "{} declared variants",
                                     id.v, childTy.v, variants.size()),
                         sourceMap_);
            }
        } else if (kind == TypeKind::Array) {
            auto const ops   = interner_->operands(aggTy);
            auto const scals = interner_->scalars(aggTy);
            if (ops.empty() || scals.empty()) {
                // Malformed Array type reaching the verifier IS the
                // lowering bug this rule was added to catch — diagnose,
                // don't silently skip.
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                         std::format("ConstructAggregate #{} (Array) on a "
                                     "malformed Array type (missing element "
                                     "type or length scalar)", id.v),
                         sourceMap_);
                continue;
            }
            std::size_t const len = static_cast<std::size_t>(scals[0]);
            if (kids.size() != len) {
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                         std::format("ConstructAggregate #{} (Array): "
                                     "expected {} children, got {}",
                                     id.v, len, kids.size()),
                         sourceMap_);
                continue;
            }
            TypeId const elemTy = ops[0];
            for (std::size_t k = 0; k < kids.size(); ++k) {
                TypeId const childTy = hir_.typeId(kids[k]);
                if (!childTy.valid()) continue;
                if (childTy.v != elemTy.v) {
                    reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                             std::format("ConstructAggregate #{} (Array) "
                                         "child[{}] type {} doesn't match "
                                         "element type {}",
                                         id.v, k, childTy.v, elemTy.v),
                             sourceMap_);
                }
            }
        } else {
            reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                     std::format("ConstructAggregate #{} on non-aggregate "
                                 "TypeKind ordinal {} (must be Struct, "
                                 "Union, or Array)",
                                 id.v, static_cast<unsigned>(kind)),
                     sourceMap_);
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
