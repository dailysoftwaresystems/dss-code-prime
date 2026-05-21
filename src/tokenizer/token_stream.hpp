#pragma once

#include "core/export.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/token.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dss {

// Iterable token container with peek/advance/rewind + opaque bookmark.
//
// Constructed only by Tokenizer (private ctor + friend). After
// `Tokenizer::tokenize() &&` returns one, the stream is positioned at
// index 0 with `peek()` = the first emitted token.
//
// Always terminates with a `CoreTokenKind::Eof` token at end-of-stream;
// `peek()` past the Eof keeps returning that Eof rather than going UB,
// so parsers can keep peeking past end without an explicit isAtEnd()
// guard.
//
// Lifetime: a TokenStream is a value type. Move-only — copying would
// give two streams pointing into the same backing vector with
// independent cursors, which is rarely what callers want.
class DSS_EXPORT TokenStream {
public:
    TokenStream() = default;
    TokenStream(TokenStream const&)            = delete;
    TokenStream& operator=(TokenStream const&) = delete;
    // Move ops are explicit (not `= default`) because every public
    // method's empty-stream guard relies on a moved-from instance
    // observably reporting `tokens_.empty() == true`. `std::vector`'s
    // defaulted-move-from state is "valid but unspecified" — most
    // implementations leave the source empty, but the standard does
    // not require it. The custom move clears the source explicitly,
    // mirroring `NodeAttribute<T>`'s move-ops discipline from SH3.
    TokenStream(TokenStream&& other) noexcept;
    TokenStream& operator=(TokenStream&& other) noexcept;

    // ── peek / advance ──
    //
    // `peek(0)` is the next token to be consumed (the same Eof appears
    // every time when at end). `peek(n)` looks ahead N tokens. Past-end
    // lookahead returns the Eof token.
    [[nodiscard]] Token const& peek(std::size_t lookahead = 0) const noexcept;

    // Consume and return the current token. At end, returns the Eof
    // token without moving past it (idempotent — same byte-span Eof
    // forever).
    Token advance() noexcept;

    [[nodiscard]] bool        isAtEnd()  const noexcept;
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] std::size_t size()     const noexcept { return tokens_.size(); }

    // Random-access set position. Out-of-range clamps to size().
    void rewind(std::size_t pos) noexcept;

    // ── bookmark / restore ──
    //
    // Opaque snapshot. Validates owner via per-instance monotonic id —
    // same pattern as LexerModeStack::Snapshot. Restore aborts on a
    // mismatched bookmark (caller bug; treating it as a soft error
    // would mask the misuse).
    class DSS_EXPORT Bookmark {
    public:
        constexpr Bookmark() noexcept = default;
        [[nodiscard]] constexpr bool valid() const noexcept { return owner_ != 0; }

    private:
        friend class TokenStream;
        constexpr Bookmark(std::uint64_t owner, std::size_t pos) noexcept
            : owner_(owner), pos_(pos) {}
        std::uint64_t owner_ = 0;
        std::size_t   pos_   = 0;
    };

    [[nodiscard]] Bookmark mark() const noexcept;
    void                   restore(Bookmark const& bm) noexcept;

private:
    friend class Tokenizer;

    // Tokenizer hands us the full vector via this private ctor. The
    // last element MUST be a CoreTokenKind::Eof token — Tokenizer
    // synthesizes one before returning. instanceId is minted by
    // Tokenizer so two streams from the same tokenize() call would
    // never share an owner (we don't currently allow that, but the
    // discipline keeps the bookmark contract clean).
    TokenStream(std::vector<Token> tokens, std::uint64_t instanceId) noexcept;

    std::vector<Token> tokens_;
    std::size_t        pos_         = 0;
    std::uint64_t      instanceId_  = 0;
};

} // namespace dss
