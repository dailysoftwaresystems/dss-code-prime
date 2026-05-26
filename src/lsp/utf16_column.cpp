#include "lsp/utf16_column.hpp"

#include <algorithm>

namespace dss::lsp {

namespace {

// Inspect the lead byte of a UTF-8 sequence to determine its length
// (1, 2, 3, or 4). Continuation bytes (`10xxxxxx`) return 0 — the
// caller treats that as a "skip forward to next code-point start"
// signal.
[[nodiscard]] std::uint8_t utf8SequenceLength(std::uint8_t lead) noexcept {
    if      ((lead & 0x80u) == 0x00u) return 1; // 0xxxxxxx — ASCII
    else if ((lead & 0xE0u) == 0xC0u) return 2; // 110xxxxx
    else if ((lead & 0xF0u) == 0xE0u) return 3; // 1110xxxx
    else if ((lead & 0xF8u) == 0xF0u) return 4; // 11110xxx
    else                              return 0; // continuation or invalid
}

} // namespace

std::uint32_t utf8ByteOffsetToUtf16Column(std::string_view lineText,
                                          std::uint32_t    byteOffsetInLine) noexcept {
    const auto clamped = std::min<std::size_t>(byteOffsetInLine, lineText.size());
    std::uint32_t utf16Count = 0;
    std::size_t   i          = 0;
    while (i < clamped) {
        const auto lead = static_cast<std::uint8_t>(lineText[i]);
        const auto seqLen = utf8SequenceLength(lead);
        if (seqLen == 0) {
            // Continuation byte / invalid lead at a position the
            // caller asked us to walk through. Best-effort recovery:
            // advance one byte, count one BMP unit. A correctly-
            // encoded source from `SourceBuffer::lineCol()` never
            // hits this branch.
            ++i;
            ++utf16Count;
            continue;
        }
        // Supplementary-plane code points (4-byte UTF-8) encode as a
        // UTF-16 surrogate pair = 2 code units; all other lengths
        // encode in 1 UTF-16 code unit (BMP).
        utf16Count += (seqLen == 4) ? 2u : 1u;
        i          += seqLen;
    }
    return utf16Count;
}

LineByteRange lineByteRangeFor(dss::SourceBuffer const& buffer,
                                std::uint32_t            byteOffset) noexcept {
    // Walk backwards from `byteOffset` to the previous line break, then
    // forward to the next. Mirrors the two divergent implementations
    // previously kept in diagnostic_translator.cpp and
    // lsp_semantic_query.cpp — consolidated here so `\r` / `\n` /
    // end-of-buffer clamping behaves identically on every path.
    const auto text = buffer.text();
    const std::uint32_t clamped =
        byteOffset <= text.size() ? byteOffset
                                  : static_cast<std::uint32_t>(text.size());
    std::uint32_t start = clamped;
    while (start > 0 && text[start - 1] != '\n' && text[start - 1] != '\r') {
        --start;
    }
    std::uint32_t end = clamped;
    while (end < text.size() && text[end] != '\n' && text[end] != '\r') {
        ++end;
    }
    return {start, end};
}

std::uint32_t utf16ColumnToByteOffset(std::string_view lineText,
                                      std::uint32_t    utf16Col) noexcept {
    std::uint32_t utf16Seen = 0;
    std::size_t   i         = 0;
    while (i < lineText.size()) {
        if (utf16Seen >= utf16Col) break;
        const auto lead   = static_cast<std::uint8_t>(lineText[i]);
        const auto seqLen = utf8SequenceLength(lead);
        if (seqLen == 0) {
            // Continuation / invalid lead: advance one byte, count one
            // unit (best-effort, mirrors the forward function).
            ++i;
            ++utf16Seen;
            continue;
        }
        const std::uint32_t units = (seqLen == 4) ? 2u : 1u;
        // A target column landing inside this code point's UTF-16 units
        // (the surrogate-pair midpoint) rounds DOWN: stop before consuming
        // the code point so we return the start of it.
        if (utf16Seen + units > utf16Col) break;
        utf16Seen += units;
        i         += seqLen;
    }
    return static_cast<std::uint32_t>(i);
}

} // namespace dss::lsp
