#pragma once

// Owns the SourceBuffer + schema + live TokenStream + lexer diagnostics
// for end-to-end tests; pump tokens via
// `b.pushToken(h.stream.advance())`.
//
// The destructor asserts `lexerDiags` is empty unless the test calls
// `dismissLexerDiags()` — happy-path tests don't have to spell that
// check out, and a regression that starts emitting unexpected
// tokenizer diagnostics surfaces on every test that wasn't told to
// expect them.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree_builder.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <ios>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace dss::tests {

class E2EHarness {
public:
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
    TokenStream                          stream;
    std::unique_ptr<DiagnosticReporter>  lexerDiags;

    E2EHarness(std::shared_ptr<SourceBuffer>        s,
               std::shared_ptr<GrammarSchema const> sch,
               TokenStream                          stm,
               std::unique_ptr<DiagnosticReporter>  diags) noexcept
        : src(std::move(s))
        , schema(std::move(sch))
        , stream(std::move(stm))
        , lexerDiags(std::move(diags)) {}

    E2EHarness(E2EHarness const&)            = delete;
    E2EHarness& operator=(E2EHarness const&) = delete;
    E2EHarness(E2EHarness&&)                 = default;
    E2EHarness& operator=(E2EHarness&&)      = default;

    // Opt out of the destructor's clean-diags assertion. Use for tests
    // that exercise tokenizer error paths and want to inspect diags
    // themselves. After dismissing, the test still owns `lexerDiags`.
    void dismissLexerDiags() noexcept { diagCheckDismissed_ = true; }

    ~E2EHarness() {
        if (diagCheckDismissed_) return;
        if (!lexerDiags) return;
        auto all = lexerDiags->all();
        if (all.empty()) return;
        ADD_FAILURE() << "E2EHarness: tokenizer produced "
                      << all.size()
                      << " unexpected diagnostic(s); the first is code 0x"
                      << std::hex << static_cast<unsigned>(all[0].code)
                      << " — call dismissLexerDiags() if this test expects them";
    }

private:
    bool diagCheckDismissed_ = false;
};

// Tokenize `sourceText` against shipped config `configName`. Schema-load
// failure is unrecoverable for an E2E test, so we GTEST_FAIL and abort
// the test immediately rather than returning a half-built harness.
[[nodiscard]] inline E2EHarness tokenizeShipped(std::string_view configName,
                                                std::string      sourceText) {
    auto loaded = GrammarSchema::loadShipped(configName);
    if (!loaded) {
        ADD_FAILURE() << "loadShipped(\"" << configName
                      << "\") failed: " << loaded.error()[0].message;
        std::abort();
    }
    auto src    = SourceBuffer::fromString(std::move(sourceText), "<e2e>");
    auto schema = *loaded;
    Tokenizer tk{src, schema};
    auto [stream, reporter] = std::move(tk).tokenize();
    return E2EHarness{
        std::move(src),
        std::move(schema),
        std::move(stream),
        std::move(reporter),
    };
}

// Drain Whitespace/Newline tokens at the head of `s` into `b`.
inline void drainWhitespace(TreeBuilder& b, TokenStream& s) {
    while (!s.isAtEnd()
        && (s.peek().coreKind == CoreTokenKind::Whitespace
         || s.peek().coreKind == CoreTokenKind::Newline)) {
        b.pushToken(s.advance());
    }
}

// Advance one token from `s` into `b`, then drain any trailing
// whitespace/newline tokens. Collapses the
// `b.pushToken(s.advance()); drainWhitespace(b, s);` pattern that
// dominates the hand-driven test bodies (40+ sites in
// `tests/core/test_tsql_subset.cpp` alone).
inline void pushNext(TreeBuilder& b, TokenStream& s) {
    b.pushToken(s.advance());
    drainWhitespace(b, s);
}

} // namespace dss::tests
