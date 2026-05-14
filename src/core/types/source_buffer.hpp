#pragma once

#include "core/export.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

// 1-based line, 1-based column. Free-standing so consumers don't need
// SourceBuffer in their headers just for this.
struct DSS_EXPORT LineCol {
    std::uint32_t line   = 0;
    std::uint32_t column = 0;
    constexpr bool operator==(LineCol const&) const noexcept = default;
};

// Owns one source file's text + a precomputed line-offset table for cheap
// byte-offset → (line, column) resolution. Shared via shared_ptr because the
// same buffer is referenced by Tree, every Token, every diagnostic.
class DSS_EXPORT SourceBuffer {
public:
    [[nodiscard]] static std::shared_ptr<SourceBuffer> fromFile(
        std::filesystem::path const& path);

    [[nodiscard]] static std::shared_ptr<SourceBuffer> fromString(
        std::string text,
        std::string name = "<string>");

    [[nodiscard]] BufferId               id()   const noexcept { return id_; }
    [[nodiscard]] std::string_view       text() const noexcept { return text_; }
    [[nodiscard]] std::string_view       name() const noexcept { return name_; }
    [[nodiscard]] std::uint32_t          size() const noexcept;

    // Returns the byte run covered by `span`. Bounds-checked: a span past
    // end-of-buffer returns the clamped portion (never UB).
    [[nodiscard]] std::string_view       slice(SourceSpan span) const noexcept;
    [[nodiscard]] std::string_view       slice(ByteOffset start, ByteOffset end) const noexcept;

    // 1-based line/column from a byte offset. Past-end offsets clamp to
    // (lineCount, lastColumn+1). Uses binary search on the line-offset table.
    [[nodiscard]] LineCol lineCol(ByteOffset byteOffset) const noexcept;

private:
    // Construction is internal — callers go through the factories so we control
    // BufferId minting.
    SourceBuffer(BufferId id, std::string text, std::string name);

    BufferId                       id_;
    std::string                    text_;
    std::string                    name_;
    std::vector<std::uint32_t>     lineStarts_;   // byte offset of each line's first char; lineStarts_[0] == 0
};

} // namespace dss
