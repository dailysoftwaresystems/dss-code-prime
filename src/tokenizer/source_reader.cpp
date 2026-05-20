#include "tokenizer/source_reader.hpp"

namespace dss {

SourceReader::SourceReader(SourceBuffer const& src) noexcept
    : src_(&src), text_(src.text()), size_(src.text().size()) {}

char SourceReader::peek(std::size_t lookahead) const noexcept {
    const std::size_t at = pos_ + lookahead;
    if (at >= size_) return '\0';
    return text_[at];
}

void SourceReader::advance(std::size_t n) noexcept {
    const std::size_t newPos = pos_ + n;
    pos_ = (newPos > size_) ? size_ : newPos;
}

std::string_view SourceReader::slice(std::size_t start,
                                     std::size_t end) const noexcept {
    if (start >= size_) return {};
    if (end > size_)    end = size_;
    if (end <= start)   return {};
    return text_.substr(start, end - start);
}

std::string_view SourceReader::remaining() const noexcept {
    if (pos_ >= size_) return {};
    return text_.substr(pos_);
}

} // namespace dss
