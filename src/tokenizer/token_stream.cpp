#include "tokenizer/token_stream.hpp"

#include <cstdio>
#include <cstdlib>

namespace dss {

namespace {

[[noreturn]] void streamFatal(char const* what) {
    std::fputs("dss::TokenStream fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

TokenStream::TokenStream(std::vector<Token> tokens,
                         std::uint64_t instanceId) noexcept
    : tokens_(std::move(tokens)), instanceId_(instanceId) {
    // Tokenizer guarantees a trailing Eof; an empty vector here would be
    // a Tokenizer bug — fail loud rather than letting peek(0) return UB
    // garbage.
    if (tokens_.empty()) streamFatal("constructed with empty token vector");
}

Token const& TokenStream::peek(std::size_t lookahead) const noexcept {
    const std::size_t at = pos_ + lookahead;
    if (at >= tokens_.size()) return tokens_.back();   // Eof
    return tokens_[at];
}

Token TokenStream::advance() noexcept {
    if (pos_ >= tokens_.size() - 1) return tokens_.back();   // Eof: idempotent
    return tokens_[pos_++];
}

bool TokenStream::isAtEnd() const noexcept {
    return pos_ >= tokens_.size() - 1;
}

void TokenStream::rewind(std::size_t pos) noexcept {
    pos_ = (pos > tokens_.size() - 1) ? tokens_.size() - 1 : pos;
}

TokenStream::Bookmark TokenStream::mark() const noexcept {
    return Bookmark{instanceId_, pos_};
}

void TokenStream::restore(Bookmark const& bm) noexcept {
    if (!bm.valid())
        streamFatal("restore on default-constructed Bookmark");
    if (bm.owner_ != instanceId_)
        streamFatal("restore on Bookmark from a different TokenStream instance");
    if (bm.pos_ > tokens_.size() - 1)
        streamFatal("restore on Bookmark with out-of-range position");
    pos_ = bm.pos_;
}

} // namespace dss
