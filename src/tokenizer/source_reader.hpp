#pragma once

#include "core/export.hpp"
#include "core/types/source_buffer.hpp"

#include <cstddef>
#include <string_view>

namespace dss {

// Buffered byte reader over a SourceBuffer. The tokenizer's only window
// into source text. UTF-8 passes through transparently — the schema's
// lexeme keys are byte strings, identifier-class predicates work on
// ASCII, and multi-byte runs in identifiers fall out of the byte-level
// consumption loop naturally.
//
// Lifetime: SourceReader holds a `SourceBuffer const*`. The caller MUST
// keep the buffer alive for as long as the reader exists (same posture
// as TreeCursor / NodeAttribute hold raw Tree pointers).
class DSS_EXPORT SourceReader {
public:
    explicit SourceReader(SourceBuffer const& src) noexcept;

    // ── position ──
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] std::size_t size()     const noexcept { return size_; }
    [[nodiscard]] bool        isAtEnd()  const noexcept { return pos_ >= size_; }

    // ── one-byte peek / advance ──
    //
    // `peek(lookahead)` returns the byte at `pos_ + lookahead`, or `\0`
    // when past end. The sentinel byte choice is convenient for char
    // predicates: `\0` is neither identifier-start nor digit, so the
    // caller doesn't need a separate isAtEnd() probe at every step.
    [[nodiscard]] char peek(std::size_t lookahead = 0) const noexcept;

    // Advance N bytes (clamped to size — past-end stays at size).
    void advance(std::size_t n = 1) noexcept;

    // ── slicing ──
    //
    // Bytes covered by [start, end). Clamped — a span past end returns
    // the in-range portion. Never UB.
    [[nodiscard]] std::string_view slice(std::size_t start,
                                         std::size_t end) const noexcept;

    // Unread tail starting at `pos_`.
    [[nodiscard]] std::string_view remaining() const noexcept;

    // Underlying buffer — exposed for callers that need to mint
    // SourceSpans tied to the same BufferId.
    [[nodiscard]] SourceBuffer const& buffer() const noexcept { return *src_; }

private:
    SourceBuffer const* src_;
    std::string_view    text_;
    std::size_t         size_ = 0;
    std::size_t         pos_  = 0;
};

} // namespace dss
