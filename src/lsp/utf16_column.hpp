#pragma once

#include "core/export.hpp"

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

} // namespace dss::lsp
