#include "core/types/source_buffer.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
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

[[noreturn]] void bufferFatal(char const* what) {
    std::fputs("dss::SourceBuffer fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// SourceSpan stores offsets as `ByteOffset` (alias for `uint32_t`) —
// see `source_span.hpp` for the 4 GiB cap documentation. A buffer that
// exceeds that cap would mint spans whose offsets silently wrap, so
// the limit must be enforced at construction. Reject loudly rather
// than let a 4-GiB-plus source mis-tokenize half-way through.
void enforceSizeLimit(std::size_t n) {
    if (n > std::numeric_limits<std::uint32_t>::max()) {
        bufferFatal("source exceeds 4 GiB ByteOffset limit");
    }
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
    auto contents = std::move(buf).str();
    enforceSizeLimit(contents.size());
    // Use new-delete-friendly construction since the constructor is private.
    return std::shared_ptr<SourceBuffer>(
        new SourceBuffer(BufferId{nextBufferIdValue()},
                         std::move(contents),
                         path.string()));
}

std::shared_ptr<SourceBuffer> SourceBuffer::fromString(std::string text, std::string name) {
    enforceSizeLimit(text.size());
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
