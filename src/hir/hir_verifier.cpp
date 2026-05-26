#include "hir/hir_verifier.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "hir/hir_node.hpp"

#include <cstdint>
#include <format>
#include <vector>

namespace dss {

namespace {

// Emit one HIR verifier diagnostic. HIR has no source spans until HR5, so the
// offending HirNodeId is stashed in the span offset: it both locates the node
// for debugging AND keeps each diagnostic's (code, buffer, span) key distinct,
// so the reporter's dedup window can't coalesce sibling violations into one.
void reportAt(DiagnosticReporter& reporter, DiagnosticCode code, HirNodeId id,
              std::string actual) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
    d.buffer   = InvalidBuffer;
    d.span     = SourceSpan::empty(id.v);
    d.actual   = std::move(actual);
    reporter.report(std::move(d));
}

} // namespace

bool HirVerifier::verify(DiagnosticReporter& reporter) const {
    std::size_t const errorsBefore = reporter.errorCount();
    checkExpressionTypes(reporter);
    checkNodeArity(reporter);
    checkBreakContinueScoping(reporter);
    // HR6 appends further rules here.
    //
    // A capped reporter (the global maxDiagnostics ceiling hit — here or in a
    // prior phase sharing this reporter) silently drops further report() calls,
    // so the error-count delta below can no longer prove "no violation". Refuse
    // to certify a clean module when the reporter can't have recorded
    // everything — never hand back a false all-clear.
    if (reporter.hitCap()) return false;
    return reporter.errorCount() == errorsBefore;
}

void HirVerifier::checkExpressionTypes(DiagnosticReporter& reporter) const {
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
                             id.v, static_cast<unsigned>(kind)));
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
                                     "clause mask", id.v, p));
                continue;
            }
            std::uint32_t const expected =
                clauseCount(static_cast<ForClause>(p)) + 1;  // +1 for the body
            if (count != expected) {
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                         std::format("ForStmt #{} has {} children but its clause mask "
                                     "implies {}", id.v, count, expected));
            }
            continue;
        }
        // CaseArm: a valued (non-default) arm must carry its match-value child.
        if (kind == HirKind::CaseArm) {
            if (!hir_.caseArmIsDefault(id) && count < 1) {
                reportAt(reporter, DiagnosticCode::H_VerifierFailure, id,
                         std::format("valued CaseArm #{} has no match-value child", id.v));
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
                                 static_cast<unsigned>(kind), id.v, bound, count));
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
                                 "depth ({} enclosing)", what, id.v, depth, targets.size()));
            continue;
        }
        // continue can only target a loop — never a switch.
        if (kind == HirKind::ContinueStmt
            && hir_.kind(targets[depth]) == HirKind::SwitchStmt) {
            reportAt(reporter, DiagnosticCode::H_InvalidBreak, id,
                     std::format("continue #{} index {} resolves to a switch; continue "
                                 "can only target a loop", id.v, depth));
        }
    }
}

} // namespace dss
