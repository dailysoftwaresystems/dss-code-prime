#include "tokenizer/source_reader.hpp"

namespace dss {

SourceReader::SourceReader(SourceBuffer const& src) noexcept
    : src_(&src), text_(src.text()), size_(src.text().size()),
      bufferSize_(src.text().size()) {}

SourceReader::SourceReader(SourceBuffer const& src, std::string_view impliedTail)
    : src_(&src), text_(src.text()), size_(src.text().size()),
      bufferSize_(src.text().size()) {
    std::string_view const text = src.text();
    if (impliedTail.empty() || text.empty() || text.ends_with(impliedTail)) {
        return;
    }
    implied_.reserve(text.size() + impliedTail.size());
    implied_.append(text);
    implied_.append(impliedTail);
    text_ = implied_;
    size_ = implied_.size();
}

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
    if (start >= bufferSize_) return {};
    if (end > bufferSize_)    end = bufferSize_;
    if (end <= start)         return {};
    return text_.substr(start, end - start);
}

std::string_view SourceReader::remaining() const noexcept {
    if (pos_ >= size_) return {};
    return text_.substr(pos_);
}

std::string_view SourceReader::remainingInBuffer() const noexcept {
    if (pos_ >= bufferSize_) return {};
    return text_.substr(pos_, bufferSize_ - pos_);
}

} // namespace dss
