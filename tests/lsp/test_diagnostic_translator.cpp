// DiagnosticTranslator: maps dss `ParseDiagnostic` (UTF-8 byte
// spans, 1-based lineCol) → LSP `Diagnostic` (0-based line +
// UTF-16 character). Hits severity mapping, message composition,
// and UTF-16 column conversion on a multi-byte line.

#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "lsp/diagnostic_translator.hpp"
#include "lsp/protocol.hpp"

#include <gtest/gtest.h>

#include <span>
#include <string>

using dss::ByteOffset;
using dss::DiagnosticCode;
using dss::DiagnosticSeverity;
using dss::ParseDiagnostic;
using dss::SourceBuffer;
using dss::SourceSpan;
using LspSeverity = dss::lsp::DiagnosticSeverity;

namespace {

[[nodiscard]] ParseDiagnostic basicDiag(ByteOffset start, ByteOffset end) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::P_UnexpectedToken;
    d.severity = DiagnosticSeverity::Error;
    d.span     = SourceSpan::of(start, end);
    d.expected = {"';'"};
    d.actual   = "'}'";
    return d;
}

} // namespace

TEST(DiagnosticTranslator, TranslatesAsciiSingleLine) {
    auto buf = SourceBuffer::fromString("let x = 1;", "t");
    auto d = basicDiag(ByteOffset{4}, ByteOffset{5});
    auto out = dss::lsp::translateDiagnostic(d, *buf);
    EXPECT_EQ(out.range.start.line, 0u);
    EXPECT_EQ(out.range.start.character, 4u);
    EXPECT_EQ(out.range.end.line, 0u);
    EXPECT_EQ(out.range.end.character, 5u);
    EXPECT_EQ(out.severity, LspSeverity::Error);
    EXPECT_EQ(out.code, "P_UnexpectedToken");
    EXPECT_EQ(out.message, "expected ';' \xE2\x80\x94 got '}'");
}

TEST(DiagnosticTranslator, MapsAllSeverities) {
    auto buf = SourceBuffer::fromString("x", "t");
    auto d = basicDiag(ByteOffset{0}, ByteOffset{1});

    d.severity = DiagnosticSeverity::Error;
    EXPECT_EQ(dss::lsp::translateDiagnostic(d, *buf).severity, LspSeverity::Error);
    d.severity = DiagnosticSeverity::Warning;
    EXPECT_EQ(dss::lsp::translateDiagnostic(d, *buf).severity, LspSeverity::Warning);
    d.severity = DiagnosticSeverity::Info;
    EXPECT_EQ(dss::lsp::translateDiagnostic(d, *buf).severity, LspSeverity::Information);
    d.severity = DiagnosticSeverity::Hint;
    EXPECT_EQ(dss::lsp::translateDiagnostic(d, *buf).severity, LspSeverity::Hint);
}

TEST(DiagnosticTranslator, ConvertsLineNumbersToZeroBased) {
    auto buf = SourceBuffer::fromString("aa\nbbb\ncccc", "t");
    // byte 7 is on line 3 (1-based) ⇒ LSP line 2.
    auto d = basicDiag(ByteOffset{7}, ByteOffset{8});
    auto out = dss::lsp::translateDiagnostic(d, *buf);
    EXPECT_EQ(out.range.start.line, 2u);
    EXPECT_EQ(out.range.start.character, 0u);
    EXPECT_EQ(out.range.end.line, 2u);
    EXPECT_EQ(out.range.end.character, 1u);
}

TEST(DiagnosticTranslator, ConvertsUtf8ColumnsToUtf16Units) {
    // "αβ" — each Greek letter is 2 UTF-8 bytes / 1 UTF-16 unit.
    // Source: "αβ;" — byte layout 0,1 = α; 2,3 = β; 4 = ';'.
    auto buf = SourceBuffer::fromString("\xCE\xB1\xCE\xB2;", "t");
    // Span over ';' at byte 4.
    auto d = basicDiag(ByteOffset{4}, ByteOffset{5});
    auto out = dss::lsp::translateDiagnostic(d, *buf);
    EXPECT_EQ(out.range.start.line, 0u);
    EXPECT_EQ(out.range.start.character, 2u)
        << "two Greek letters consume 4 UTF-8 bytes but 2 UTF-16 units";
    EXPECT_EQ(out.range.end.character, 3u);
}

TEST(DiagnosticTranslator, SerializePublishDiagnosticsIncludesUriVersionAndArray) {
    dss::lsp::PublishDiagnosticsParams params;
    params.uri     = "file:///a.toy";
    params.version = 7;
    dss::lsp::Diagnostic d;
    d.range.start = {0u, 1u};
    d.range.end   = {0u, 2u};
    d.severity    = LspSeverity::Warning;
    d.code        = "P_UnexpectedToken";
    d.source      = "dss-code-prime";
    d.message     = "hello";
    params.diagnostics.push_back(d);
    const auto wire = dss::lsp::serializePublishDiagnostics(params);
    EXPECT_NE(wire.find(R"("uri":"file:///a.toy")"), std::string::npos);
    EXPECT_NE(wire.find(R"("version":7)"), std::string::npos);
    EXPECT_NE(wire.find(R"("severity":2)"), std::string::npos);
    EXPECT_NE(wire.find(R"("code":"P_UnexpectedToken")"), std::string::npos);
    EXPECT_NE(wire.find(R"("source":"dss-code-prime")"), std::string::npos);
    EXPECT_NE(wire.find(R"("message":"hello")"), std::string::npos);
}
