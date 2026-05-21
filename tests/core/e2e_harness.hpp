#pragma once

// Shared end-to-end harness for tests that drive the live
// tokenize → resolve → build pipeline against a shipped `.lang.json`
// config. Each `tokenizeShipped(configName, source)` call returns the
// owned SourceBuffer + schema pointer + a freshly-tokenized
// TokenStream + the tokenizer's DiagnosticReporter.
//
// Tests pump tokens into TreeBuilder via `b.pushToken(h.stream.advance())`
// until the stream's Eof. Pre-TZ1 the same role was played by a
// `TokenSeq` helper that hand-fabricated tokens from source-text
// substrings; TZ1 onward this is the canonical pattern.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree_builder.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace dss::tests {

struct E2EHarness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
    TokenStream                          stream;
    std::unique_ptr<DiagnosticReporter>  lexerDiags;
};

[[nodiscard]] inline E2EHarness tokenizeShipped(std::string_view configName,
                                                std::string      sourceText) {
    auto loaded = GrammarSchema::loadShipped(configName);
    auto src    = SourceBuffer::fromString(std::move(sourceText), "<e2e>");
    if (!loaded) {
        ADD_FAILURE() << "loadShipped(\"" << configName
                      << "\") failed — config missing or malformed";
        return E2EHarness{.src = std::move(src), .schema = nullptr};
    }
    auto schema = *loaded;
    Tokenizer tk{src, schema};
    auto [stream, reporter] = std::move(tk).tokenize();
    return E2EHarness{
        .src        = std::move(src),
        .schema     = std::move(schema),
        .stream     = std::move(stream),
        .lexerDiags = std::move(reporter),
    };
}

// Drain whitespace/newline tokens at the front of `s` straight into
// `b`, stopping before the next meaningful token (or at Eof). The
// AST cursor skips EmptySpace nodes so the structural assertions in
// `prettyPrint`-based tests are unaffected by interleaved whitespace.
// Tests use this between structural opens to keep the
// pushToken-per-source-position cadence readable.
inline void drainWhitespace(TreeBuilder& b, TokenStream& s) {
    while (!s.isAtEnd()
        && (s.peek().coreKind == CoreTokenKind::Whitespace
         || s.peek().coreKind == CoreTokenKind::Newline)) {
        b.pushToken(s.advance());
    }
}

} // namespace dss::tests
