#include "lsp/diagnostic_translator.hpp"

#include "core/types/source_span.hpp"
#include "lsp/utf16_column.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace dss::lsp {

namespace {

using json = nlohmann::json;

// Severity mapping. The dss enum has Hint=0/Info=1/Warning=2/Error=3
// (ascending); LSP uses Error=1/Warning=2/Information=3/Hint=4.
[[nodiscard]] DiagnosticSeverity mapSeverity(dss::DiagnosticSeverity s) noexcept {
    switch (s) {
        case dss::DiagnosticSeverity::Error:   return DiagnosticSeverity::Error;
        case dss::DiagnosticSeverity::Warning: return DiagnosticSeverity::Warning;
        case dss::DiagnosticSeverity::Info:    return DiagnosticSeverity::Information;
        case dss::DiagnosticSeverity::Hint:    return DiagnosticSeverity::Hint;
    }
    return DiagnosticSeverity::Error;
}

// Convert a 1-based byte column from `SourceBuffer::lineCol` into a
// 0-based UTF-16 character column. Uses the shared lineByteRangeFor
// helper in `utf16_column.hpp` so every LSP line-resolution path
// agrees on `\r`/`\n`/EOB clamping.
[[nodiscard]] std::uint32_t lspCharacter(dss::SourceBuffer const& buffer,
                                          std::uint32_t            byteOffset) {
    const auto range = lineByteRangeFor(buffer, byteOffset);
    const auto lineText = buffer.text().substr(
        range.startByte, range.endByte - range.startByte);
    const std::uint32_t byteCol = (byteOffset >= range.startByte)
        ? (byteOffset - range.startByte)
        : 0;
    return utf8ByteOffsetToUtf16Column(lineText, byteCol);
}

// Render a code as the wire string the LSP client sees (e.g.
// "P_UnexpectedToken"). Uses the existing diagnostic-code → name
// helper from core.
[[nodiscard]] std::string codeName(dss::DiagnosticCode code) {
    auto sv = dss::diagnosticCodeName(code);
    return std::string{sv};
}

// Build the human-readable message from `actual`/`expected`/`note`.
// Matches the spirit of `DiagnosticReporter::format` but without
// caret/line context (LSP clients render those themselves via the
// range).
[[nodiscard]] std::string composeMessage(dss::ParseDiagnostic const& d) {
    std::string msg;
    if (!d.expected.empty()) {
        msg += "expected ";
        for (std::size_t i = 0; i < d.expected.size(); ++i) {
            if (i > 0) msg += (i + 1 == d.expected.size()) ? " or " : ", ";
            msg += d.expected[i];
        }
    }
    if (!d.actual.empty()) {
        if (!msg.empty()) msg += " — ";
        msg += "got ";
        msg += d.actual;
    }
    if (!d.suggestion.empty()) {
        if (!msg.empty()) msg += ": ";
        msg += d.suggestion;
    }
    if (msg.empty()) {
        // Fallback to the bare code name so the user sees something.
        msg = codeName(d.code);
    }
    return msg;
}

} // namespace

Diagnostic translateDiagnostic(dss::ParseDiagnostic const& pd,
                                dss::SourceBuffer const&    buffer) {
    const auto startLc = buffer.lineCol(pd.span.start());
    const auto endLc   = buffer.lineCol(pd.span.end());

    Diagnostic out;
    out.range.start.line      = (startLc.line   > 0) ? startLc.line   - 1 : 0;
    out.range.start.character = lspCharacter(buffer, pd.span.start());
    out.range.end.line        = (endLc.line     > 0) ? endLc.line     - 1 : 0;
    out.range.end.character   = lspCharacter(buffer, pd.span.end());
    out.severity              = mapSeverity(pd.severity);
    out.code                  = codeName(pd.code);
    out.message               = composeMessage(pd);
    return out;
}

std::vector<Diagnostic> translateDiagnostics(
    std::span<dss::ParseDiagnostic const> diags,
    dss::SourceBuffer const&              buffer) {
    std::vector<Diagnostic> out;
    out.reserve(diags.size());
    for (auto const& d : diags) {
        out.push_back(translateDiagnostic(d, buffer));
    }
    return out;
}

std::string serializePublishDiagnostics(PublishDiagnosticsParams const& params) {
    json arr = json::array();
    for (auto const& d : params.diagnostics) {
        json item;
        item["range"] = {
            {"start", {{"line", d.range.start.line},
                       {"character", d.range.start.character}}},
            {"end",   {{"line", d.range.end.line},
                       {"character", d.range.end.character}}},
        };
        item["severity"] = static_cast<int>(d.severity);
        item["code"]     = d.code;
        item["source"]   = d.source;
        item["message"]  = d.message;
        arr.push_back(std::move(item));
    }
    json obj;
    obj["uri"]         = params.uri;
    if (params.version.has_value()) obj["version"] = *params.version;
    obj["diagnostics"] = std::move(arr);
    return obj.dump();
}

} // namespace dss::lsp
