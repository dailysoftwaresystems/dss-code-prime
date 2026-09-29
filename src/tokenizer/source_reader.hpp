#pragma once

#include "core/export.hpp"
#include "core/types/source_buffer.hpp"

#include <cstddef>
#include <string>
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
//
// ★★ TWO COORDINATE SYSTEMS, AND WHICH MEMBER SPEAKS WHICH. A reader may read
// MORE bytes than its buffer holds: the IMPLIED TAIL (see the second
// constructor). Everything that READS — `peek`, `advance`, `remaining`,
// `isAtEnd` — covers the tail. Everything that hands out a COORDINATE —
// `position`, `size`, `slice` — is in the BUFFER's coordinates, clamped to its
// end. That split is what lets a token the tail produced exist without a span
// past the buffer: its start and end both clamp to the end of the buffer, so it
// is zero-width there, exactly like the `Eof` token. Every downstream consumer
// slices the BUFFER by a token's span and so never sees a byte the buffer does
// not hold.
class DSS_EXPORT SourceReader {
public:
    explicit SourceReader(SourceBuffer const& src) noexcept;

    // ★★★ A READER OVER THE TEXT AS IF IT ENDED WITH `impliedTail` — the lexeme
    // a language declares that the END OF ITS INPUT IMPLIES (`endOfInputImplies`
    // in a `.lang.json`). The tail is read only when it is non-empty, the text is
    // non-empty, and the text does not already end with it; otherwise this
    // reader is exactly the one-argument one. An empty text stays empty.
    //
    // This is GNU as's rule for a line-oriented language, stated as data:
    // ✔MEASURED 2026-09-23, GNU as 2.42 (x86_64, aarch64, mingw-w64) accepts a
    // `.s` whose last line has no newline, saying "end of file not at end of a
    // line; newline inserted", and clang 18.1.3 accepts it silently. The user's
    // buffer is never rewritten: the tail exists only inside this reader.
    SourceReader(SourceBuffer const& src, std::string_view impliedTail);

    // `text_` may view `implied_`, so a copy or a move would dangle. The reader
    // is a local of `Tokenizer::tokenize()` and is passed by reference.
    SourceReader(SourceReader const&)            = delete;
    SourceReader& operator=(SourceReader const&) = delete;
    SourceReader(SourceReader&&)                 = delete;
    SourceReader& operator=(SourceReader&&)      = delete;

    // ── position — BUFFER coordinates ──
    [[nodiscard]] std::size_t position() const noexcept {
        return pos_ < bufferSize_ ? pos_ : bufferSize_;
    }
    [[nodiscard]] std::size_t size()     const noexcept { return bufferSize_; }
    // ── the READ extent — the implied tail included ──
    [[nodiscard]] bool        isAtEnd()  const noexcept { return pos_ >= size_; }
    // ── no byte of the BUFFER is left (an implied tail may be) ──
    // ★ THE END OF INPUT, as far as anything the author WROTE is concerned. A
    // body (a comment, a literal) must never read into the implied tail: the
    // tail is a line end the language implies, not text inside the body — so a
    // body scan stops HERE, and the tokenizer's end-of-input verdict for every
    // frame still open is taken here, before the tail is lexed.
    [[nodiscard]] bool        atBufferEnd() const noexcept { return pos_ >= bufferSize_; }

    // ── one-byte peek / advance ──
    //
    // `peek(lookahead)` returns the byte at `pos_ + lookahead`, or `\0`
    // when past end. The sentinel byte choice is convenient for char
    // predicates: `\0` is neither identifier-start nor digit, so the
    // caller doesn't need a separate isAtEnd() probe at every step.
    [[nodiscard]] char peek(std::size_t lookahead = 0) const noexcept;

    // Advance N bytes (clamped to the read extent — past-end stays at end).
    void advance(std::size_t n = 1) noexcept;

    // ── slicing — BUFFER coordinates ──
    //
    // Bytes covered by [start, end). Clamped to the BUFFER — a span past its
    // end returns the in-range portion, so an implied tail is never part of a
    // slice. Never UB.
    [[nodiscard]] std::string_view slice(std::size_t start,
                                         std::size_t end) const noexcept;

    // Unread tail starting at `pos_` — the implied tail included, so a lexeme
    // match at the end of the buffer sees exactly the bytes a newline-terminated
    // file would have had there.
    [[nodiscard]] std::string_view remaining() const noexcept;

    // The unread BUFFER bytes only — what a BODY's closer or escape may match
    // against. A body never reads into an implied tail (see `atBufferEnd`), and
    // that must hold for a multi-byte closer too, which `remaining()` would let
    // straddle the end of the buffer into the tail.
    [[nodiscard]] std::string_view remainingInBuffer() const noexcept;

    // Underlying buffer — exposed for callers that need to mint
    // SourceSpans tied to the same BufferId.
    [[nodiscard]] SourceBuffer const& buffer() const noexcept { return *src_; }

private:
    SourceBuffer const* src_;
    std::string         implied_;         // buffer text + implied tail, when one applies
    std::string_view    text_;            // what is READ: the buffer's text or `implied_`
    std::size_t         size_       = 0;  // bytes READ
    std::size_t         bufferSize_ = 0;  // bytes the BUFFER holds — every coordinate's bound
    std::size_t         pos_        = 0;
};

} // namespace dss
