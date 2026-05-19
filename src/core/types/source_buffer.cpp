#include "core/types/source_buffer.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace dss {

namespace {

// Monotonic BufferId counter. Starts at 1 so 0 stays the InvalidBuffer sentinel.
std::uint32_t nextBufferIdValue() {
    static std::atomic<std::uint32_t> counter{0};
    return ++counter;
}

// Precomputes byte offsets of each line start. Handles \n, \r\n, and lone \r
// (treating \r\n as one line break and a lone \r as a line break for Mac classic files).
std::vector<std::uint32_t> buildLineStarts(std::string_view text) {
    std::vector<std::uint32_t> starts;
    starts.reserve(text.size() / 32 + 1);   // ~32 chars/line heuristic
    starts.push_back(0);
    for (std::uint32_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n') {
            starts.push_back(i + 1);
        } else if (c == '\r') {
            // Don't double-count \r\n.
            const bool isCrLf = (i + 1 < text.size()) && (text[i + 1] == '\n');
            if (!isCrLf) {
                starts.push_back(i + 1);
            }
        }
    }
    return starts;
}

} // namespace

SourceBuffer::SourceBuffer(BufferId id, std::string text, std::string name)
    : id_(id)
    , text_(std::move(text))
    , name_(std::move(name))
    , lineStarts_(buildLineStarts(text_)) {}

std::shared_ptr<SourceBuffer> SourceBuffer::fromFile(std::filesystem::path const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("SourceBuffer::fromFile: cannot open " + path.string());
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    // Use new-delete-friendly construction since the constructor is private.
    return std::shared_ptr<SourceBuffer>(
        new SourceBuffer(BufferId{nextBufferIdValue()},
                         std::move(buf).str(),
                         path.string()));
}

std::shared_ptr<SourceBuffer> SourceBuffer::fromString(std::string text, std::string name) {
    return std::shared_ptr<SourceBuffer>(
        new SourceBuffer(BufferId{nextBufferIdValue()},
                         std::move(text),
                         std::move(name)));
}

std::uint32_t SourceBuffer::size() const noexcept {
    return static_cast<std::uint32_t>(text_.size());
}

std::string_view SourceBuffer::slice(SourceSpan span) const noexcept {
    return slice(span.start(), span.end());
}

std::string_view SourceBuffer::slice(ByteOffset start, ByteOffset end) const noexcept {
    const std::uint32_t n = size();
    const std::uint32_t s = std::min(start, n);
    const std::uint32_t e = std::min(end, n);
    if (e <= s) return {};
    return std::string_view{text_}.substr(s, e - s);
}

LineCol SourceBuffer::lineCol(ByteOffset byteOffset) const noexcept {
    // Clamp past-end offsets so callers can pass span.end() (one-past) safely.
    const std::uint32_t off = std::min(byteOffset, size());

    // Binary search for the largest line-start offset <= off.
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), off);
    // `it` points one past the matching line; back up.
    const auto lineIndex = static_cast<std::uint32_t>(std::distance(lineStarts_.begin(), it) - 1);
    const std::uint32_t lineStart = lineStarts_[lineIndex];
    return LineCol{
        .line   = lineIndex + 1,                  // 1-based
        .column = (off - lineStart) + 1,          // 1-based
    };
}

} // namespace dss
