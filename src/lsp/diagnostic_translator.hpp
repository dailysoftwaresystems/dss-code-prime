#pragma once

#include "core/export.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "lsp/protocol.hpp"

#include <span>
#include <string>
#include <vector>

// Translate `ParseDiagnostic` (UTF-8 byte spans, 1-based line/col
// from `SourceBuffer::lineCol`) into `lsp::Diagnostic` (0-based
// line/UTF-16 character). Stateless — every call recomputes from
// the buffer. `SourceBuffer::lineCol` is O(log lines); UTF-16
// conversion is O(line length per diagnostic) — fast enough for
// LSP scenarios that report tens of diagnostics per parse.

namespace dss::lsp {

// Translate one diagnostic. `buffer` MUST be the SourceBuffer the
// diagnostic was emitted against — the lineCol mapping is
// buffer-specific. Returns a fully populated LSP Diagnostic ready
// to be serialized as part of a `publishDiagnostics` notification.
[[nodiscard]] DSS_EXPORT Diagnostic translateDiagnostic(
    dss::ParseDiagnostic const& pd,
    dss::SourceBuffer const&    buffer);

// Bulk translation: iterates the span and calls `translateDiagnostic`
// for each. The caller is responsible for ensuring the span outlives
// this call — i.e., copy out of `tree.diagnostics().all()` before
// the Tree dies.
[[nodiscard]] DSS_EXPORT std::vector<Diagnostic> translateDiagnostics(
    std::span<dss::ParseDiagnostic const> diags,
    dss::SourceBuffer const&              buffer);

// Serialize a `PublishDiagnosticsParams` to a JSON string suitable
// for `JsonRpc::serializeNotification`. nlohmann is used in the
// `.cpp` only.
[[nodiscard]] DSS_EXPORT std::string serializePublishDiagnostics(
    PublishDiagnosticsParams const& params);

} // namespace dss::lsp
