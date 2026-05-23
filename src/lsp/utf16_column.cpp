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

} // namespace dss::lsp
