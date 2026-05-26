#pragma once

#include "core/export.hpp"
#include "core/types/source_buffer.hpp"

#include <cstdint>
#include <string_view>

// UTF-8 byte offset → UTF-16 code unit conversion. The LSP protocol
// (3.16+) defaults to UTF-16 for `Position.character`; the parser's
// `SourceSpan` carries UTF-8 byte offsets. This single function is
// the only place UTF-16 arithmetic appears in the codebase.
//
// `lineText` is the UTF-8 source text of ONE line (no embedded
// newlines). `byteOffsetInLine` is the 0-based UTF-8 byte offset
// within that line. The return is the corresponding 0-based UTF-16
// code unit offset:
//   - ASCII (1 UTF-8 byte)        → 1 UTF-16 unit
//   - 2-byte UTF-8 (U+0080..07FF) → 1 UTF-16 unit
//   - 3-byte UTF-8 (U+0800..FFFF) → 1 UTF-16 unit
//   - 4-byte UTF-8 (U+10000+, supplementary plane) → 2 UTF-16 units
//     (surrogate pair)
//
// `byteOffsetInLine` past the end of `lineText` is clamped to the
// line length. Malformed UTF-8 (a byte position that lands inside a
// multi-byte sequence) is rounded UP to the next code-point start —
// a graceful fallback rather than an abort, since LSP positions are
// best-effort hints. Continuation bytes at the start of `lineText`
// (rare; would indicate a buffer offset error upstream) are skipped.

namespace dss::lsp {

[[nodiscard]] DSS_EXPORT std::uint32_t utf8ByteOffsetToUtf16Column(
    std::string_view lineText,
    std::uint32_t    byteOffsetInLine) noexcept;

// Inverse of the above: given a line's UTF-8 text and a 0-based UTF-16
// code-unit column (an LSP `Position.character`), return the
// corresponding 0-based UTF-8 BYTE offset within that line. Used to map
// an LSP position back onto the parser's byte-span world.
//
// A `utf16Col` past the end of the line clamps to the line length. A
// column that lands in the MIDDLE of a surrogate pair (4-byte UTF-8 code
// point) rounds DOWN to the start of that code point — the same
// best-effort tolerance the forward function applies, since LSP positions
// are hints. Continuation bytes are never returned as an offset.
[[nodiscard]] DSS_EXPORT std::uint32_t utf16ColumnToByteOffset(
    std::string_view lineText,
    std::uint32_t    utf16Col) noexcept;

// The UTF-8 byte range [startByte, endByte) of the source line
// containing `byteOffset` in `buffer`. `endByte` is exclusive of the
// terminating `\n` / `\r` (or end-of-buffer for the last line without
// a trailing newline). One canonical helper consumed by the
// diagnostic translator AND the LSP semantic query layer — keeping
// `\r` handling consistent across both. Walks backwards from
// `byteOffset` to the previous line break, then forward to the next.
// Clamps `byteOffset` to the buffer length.
struct LineByteRange {
    std::uint32_t startByte;
    std::uint32_t endByte;
};

[[nodiscard]] DSS_EXPORT LineByteRange lineByteRangeFor(
    dss::SourceBuffer const& buffer,
    std::uint32_t            byteOffset) noexcept;

} // namespace dss::lsp
