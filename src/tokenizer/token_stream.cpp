#include "tokenizer/token_stream.hpp"

#include <atomic>

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
    // Belt-and-braces: every Tokenizer code path appends a trailing
    // Eof before constructing this stream, but an upstream bug that
    // appends the wrong kind would otherwise leak into the parser as a
    // silent peek-past-end shape change. The check is cheap (one enum
    // compare on a small back element) and pinned by `peek_past_eof`-
    // style tests.
    if (tokens_.back().coreKind != CoreTokenKind::Eof)
        streamFatal("constructed with non-Eof trailing token — Tokenizer bug");
}

TokenStream TokenStream::fromTokens(std::vector<Token> tokens) {
    // Independent instance-id counter, tagged in the high bit so a PP-built
    // stream's id can never equal a Tokenizer-built stream's id (the two use
    // separate counters; the tag makes the disjointness explicit). Bookmarks
    // are validated per-instance, so this guarantees a bookmark from one
    // stream is rejected by the other.
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t id =
        (std::uint64_t{1} << 63) | (++counter);
    // The (vector, id) ctor fatal-asserts non-empty + trailing-Eof, so the
    // PP's own contract (it always appends an Eof) is double-checked here.
    return TokenStream{std::move(tokens), id};
}

TokenStream::TokenStream(TokenStream&& other) noexcept
    : tokens_(std::move(other.tokens_))
    , pos_(other.pos_)
    , instanceId_(other.instanceId_) {
    other.tokens_.clear();
    other.pos_        = 0;
    other.instanceId_ = 0;
}

TokenStream& TokenStream::operator=(TokenStream&& other) noexcept {
    if (this == &other) return *this;
    tokens_     = std::move(other.tokens_);
    pos_        = other.pos_;
    instanceId_ = other.instanceId_;
    other.tokens_.clear();
    other.pos_        = 0;
    other.instanceId_ = 0;
    return *this;
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
