#include "hir/hir_verifier.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "hir/hir_node.hpp"

#include <cstdint>
#include <format>

namespace dss {

bool HirVerifier::verify(DiagnosticReporter& reporter) const {
    std::size_t const errorsBefore = reporter.errorCount();
    checkExpressionTypes(reporter);
    // HR6 appends further rules here.
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

        ParseDiagnostic d;
        d.code     = DiagnosticCode::H_TypeUnresolved;
        d.severity = DiagnosticSeverity::Error;
        // HIR has no source spans until HR5; stash the offending HirNodeId in the
        // span offset. This both locates the node for debugging AND keeps each
        // diagnostic's (code, buffer, span) key distinct, so the reporter's dedup
        // window doesn't coalesce sibling untyped-node violations into one.
        d.buffer = InvalidBuffer;
        d.span   = SourceSpan::empty(id.v);
        d.actual = std::format("hir node #{} (HirKind ordinal {})",
                               id.v, static_cast<unsigned>(kind));
        reporter.report(std::move(d));
    }
}

} // namespace dss
