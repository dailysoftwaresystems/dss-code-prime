#pragma once

#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/token.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// Shared test fixture for builder/cursor tests. Owns a `SourceBuffer`
// and a `GrammarSchema` loaded from a caller-supplied JSON config. The
// `tok` helpers synthesize `Token` objects pointing into the source buffer.

namespace dss::tests {

struct ToyHarness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;

    // Construct from source text + a JSON config (typically a `R"JSON(...)JSON"`
    // literal stored as a `std::string_view` constant in the test file).
    static ToyHarness make(std::string sourceText, std::string_view configText,
                           std::string sourceLabel = "<test>") {
        ToyHarness h;
        h.src = SourceBuffer::fromString(std::move(sourceText), std::move(sourceLabel));
        auto loaded = GrammarSchema::loadFromText(configText);
        EXPECT_TRUE(loaded.has_value())
            << (loaded.has_value() ? "" : loaded.error()[0].message);
        h.schema = *loaded;
        return h;
    }

    // Build a Token from a substring of the source. The substring MUST
    // appear exactly once — otherwise we'd silently use the first hit and
    // confuse later tests that expect different positions. Use the
    // `startHint` overload for sources with repeated lexemes.
    Token tok(std::string_view text, CoreTokenKind kind = CoreTokenKind::Operator) const {
        const auto sv = src->text();
        const auto first = sv.find(text);
        EXPECT_NE(first, std::string_view::npos) << "test source missing '" << text << "'";
        const auto second = (first == std::string_view::npos)
            ? std::string_view::npos
            : sv.find(text, first + 1);
        EXPECT_EQ(second, std::string_view::npos)
            << "ToyHarness::tok: '" << text
            << "' appears multiple times in the source; use the startHint overload";
        return Token{
            .coreKind   = kind,
            .schemaKind = InvalidSchemaToken,
            .span       = SourceSpan::of(static_cast<ByteOffset>(first),
                                          static_cast<ByteOffset>(first + text.size())),
        };
    }

    // Build a Token at an explicit byte offset. Use when the same lexeme
    // appears multiple times in the source.
    Token tok(std::string_view text, std::size_t startHint, CoreTokenKind kind) const {
        return Token{
            .coreKind   = kind,
            .schemaKind = InvalidSchemaToken,
            .span       = SourceSpan::of(static_cast<ByteOffset>(startHint),
                                          static_cast<ByteOffset>(startHint + text.size())),
        };
    }
};

} // namespace dss::tests
