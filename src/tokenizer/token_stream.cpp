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
    if (instanceId_ == 0)
        streamFatal("constructed with zero instance id — Tokenizer bug");
}

// A default-constructed or moved-from stream has empty `tokens_`,
// which would make every `size() - 1` arithmetic UB. Each method
// short-circuits via this helper. Public default ctor is preserved
// because callers (test harnesses, optional-style aggregates) need to
// declare a stream variable before assigning the Tokenizer's result
// into it.
namespace {
[[noreturn]] void emptyStreamFatal() {
    streamFatal("operation on default-constructed or moved-from TokenStream");
}
} // namespace

Token const& TokenStream::peek(std::size_t lookahead) const noexcept {
    if (tokens_.empty()) emptyStreamFatal();
    const std::size_t at = pos_ + lookahead;
    if (at >= tokens_.size()) return tokens_.back();   // Eof
    return tokens_[at];
}

Token TokenStream::advance() noexcept {
    if (tokens_.empty()) emptyStreamFatal();
    if (pos_ >= tokens_.size() - 1) return tokens_.back();   // Eof: idempotent
    return tokens_[pos_++];
}

bool TokenStream::isAtEnd() const noexcept {
    if (tokens_.empty()) emptyStreamFatal();
    return pos_ >= tokens_.size() - 1;
}

void TokenStream::rewind(std::size_t pos) noexcept {
    if (tokens_.empty()) emptyStreamFatal();
    pos_ = (pos > tokens_.size() - 1) ? tokens_.size() - 1 : pos;
}

TokenStream::Bookmark TokenStream::mark() const noexcept {
    if (tokens_.empty()) emptyStreamFatal();
    return Bookmark{instanceId_, pos_};
}

void TokenStream::restore(Bookmark const& bm) noexcept {
    if (tokens_.empty()) emptyStreamFatal();
    if (!bm.valid())
        streamFatal("restore on default-constructed Bookmark");
    if (bm.owner_ != instanceId_)
        streamFatal("restore on Bookmark from a different TokenStream instance");
    if (bm.pos_ > tokens_.size() - 1)
        streamFatal("restore on Bookmark with out-of-range position");
    pos_ = bm.pos_;
}

} // namespace dss
