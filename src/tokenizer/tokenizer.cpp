#include "tokenizer/tokenizer.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "tokenizer/source_reader.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <format>
#include <utility>

namespace dss {

namespace {

// ── char predicates ──────────────────────────────────────────────────────
//
// Tokenizer is ASCII-aware at this level — UTF-8 bytes >127 are passed
// through transparently in identifier runs (the schema's lexeme keys are
// byte strings; a multi-byte UTF-8 identifier matches lexeme keys
// byte-for-byte). The isAsciiX predicates are intentional: extending to
// Unicode XID_Start / XID_Continue is a v3-class concern, not TZ1's.

[[nodiscard]] constexpr bool isAsciiSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r';
}

[[nodiscard]] constexpr bool isIdStart(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_' || u >= 0x80;
}

[[nodiscard]] constexpr bool isIdContinue(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return isIdStart(c) || (u >= '0' && u <= '9');
}

[[nodiscard]] constexpr bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// ASCII punctuation marks that the tokenizer flags as Punctuation rather
// than Operator. The split is purely a coreKind hint — the schema's
// resolved meaning is what the builder consumes. Keeping the set narrow
// makes the operator-vs-punctuation distinction predictable.
[[nodiscard]] constexpr bool isAsciiPunctuation(char c) noexcept {
    return c == '(' || c == ')' || c == '{' || c == '}'
        || c == '[' || c == ']' || c == ',' || c == ';'
        || c == ':' || c == '.';
}

[[nodiscard]] constexpr CoreTokenKind coreKindForByte(char c) noexcept {
    if (c == '\n')           return CoreTokenKind::Newline;
    if (isAsciiSpace(c))     return CoreTokenKind::Whitespace;
    if (isAsciiPunctuation(c)) return CoreTokenKind::Punctuation;
    return CoreTokenKind::Operator;
}

// Stamp a unique instance id on every TokenStream so bookmarks taken
// from one can't accidentally restore on another. Same idiom as
// LexerModeStack::Snapshot's owner stamp.
[[nodiscard]] std::uint64_t nextStreamInstanceId() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return ++counter;
}

// ── lexeme lookup ─────────────────────────────────────────────────────────
//
// TZ1 ships with a fixed cap on tried lexeme lengths. c-subset's longest
// schema-declared lexeme is 2 chars (`==`, `!=`, `<=`, `>=`, `&&`, `||`,
// `<<`, `>>`); tsql-subset will surface 2-char prefixes (`N'`). Four is
// comfortable headroom for TZ2/TZ3 without needing schema-side key
// enumeration. The cap is reviewed if a config declares anything longer
// (no current schema does).

constexpr std::size_t kMaxLexemeLength = 4;

struct LookupHit {
    std::size_t      length = 0;       // bytes consumed; 0 = no match
    LexemeMeaning    meaning{};        // the winning candidate
};

[[nodiscard]] LookupHit longestMatch(GrammarSchema const& schema,
                                     std::string_view remaining) noexcept {
    const std::size_t maxN = std::min(kMaxLexemeLength, remaining.size());
    for (std::size_t len = maxN; len >= 1; --len) {
        const auto cands = schema.lookupLexeme(remaining.substr(0, len));
        if (cands.empty()) continue;
        // `lookupLexeme` returns candidates pre-sorted by priority
        // (lowest first, stable on declaration order ties) per the
        // GrammarSchema contract. First entry is the winner.
        return LookupHit{ .length = len, .meaning = cands[0] };
    }
    return {};
}

// ── numeric scan ──────────────────────────────────────────────────────────
//
// Hand-coded per `.plans/tokenizer-plan.md` §2.4. The grammar:
//
//   int    := [0-9]+ suffix?
//   float  := ([0-9]+ \. [0-9]*  |  \. [0-9]+  |  [0-9]+ exponent) suffix?
//   suffix := [a-zA-Z_]+
//
// The dot-only-prefix case (`.5`) is handled by the *caller* — by the
// time we enter scanNumber the cursor is already on a digit. Float
// without exponent OR fractional part is still tagged Float when an
// alphabetic suffix marks it (e.g. `0f`); otherwise int.
//
// `outIsFloat` is set true when any of: `.` consumed, `e`/`E` consumed,
// OR a float-marker suffix character (`f`/`F`/`d`/`D`) appears as the
// first char of the suffix run. Caller emits FloatLiteral vs IntLiteral
// accordingly.

void scanNumber(SourceReader& r, bool& outIsFloat) noexcept {
    outIsFloat = false;
    while (isDigit(r.peek())) r.advance(1);

    // Fractional part — but only if the next char is `.` AND the char
    // after that is a digit (avoids gobbling `a.b` member access).
    if (r.peek() == '.' && isDigit(r.peek(1))) {
        outIsFloat = true;
        r.advance(1);                                  // '.'
        while (isDigit(r.peek())) r.advance(1);
    }

    // Exponent.
    bool sawEButNoExp = false;
    if (r.peek() == 'e' || r.peek() == 'E') {
        // `1e+5` / `1e-5` / `1e5` all valid; bare `1e` (no digit) is
        // not. When the exponent fails, mark the `e` as off-limits to
        // the suffix scan so it stays available as a fresh-start
        // identifier for the next tokenize iteration.
        std::size_t look = 1;
        if (r.peek(1) == '+' || r.peek(1) == '-') look = 2;
        if (isDigit(r.peek(look))) {
            outIsFloat = true;
            r.advance(look + 1);
            while (isDigit(r.peek())) r.advance(1);
        } else {
            sawEButNoExp = true;
        }
    }

    // Suffix (any trailing ASCII letter run). `f`/`F`/`d`/`D` at the
    // start of the suffix promote an otherwise-int literal to float.
    // Skipped when we just bailed on a non-exponent `e`/`E` — that
    // char belongs to the next token, not this number's suffix.
    if (!sawEButNoExp && isIdStart(r.peek())) {
        const char first = r.peek();
        if (first == 'f' || first == 'F' || first == 'd' || first == 'D') {
            outIsFloat = true;
        }
        while (isIdContinue(r.peek())) r.advance(1);
    }
}

} // namespace

Tokenizer::Tokenizer(std::shared_ptr<SourceBuffer>        src,
                     std::shared_ptr<GrammarSchema const> schema,
                     DiagnosticReporter::Config           diagConfig)
    : source_(std::move(src))
    , schema_(std::move(schema))
    , reporter_(std::make_unique<DiagnosticReporter>(std::move(diagConfig))) {
    if (!source_) {
        std::fputs("dss::Tokenizer fatal: source is null\n", stderr);
        std::abort();
    }
    if (!schema_) {
        std::fputs("dss::Tokenizer fatal: schema is null\n", stderr);
        std::abort();
    }
}

std::pair<TokenStream, std::unique_ptr<DiagnosticReporter>>
Tokenizer::tokenize() && {
    SourceReader r{*source_};
    std::vector<Token> tokens;

    // Pre-resolve the built-in literal kinds once. The schema's
    // SchemaTokenInterner pre-interns these at load time (see T4 +
    // grammar_schema.cpp's startup path); `find` returns the existing
    // id or InvalidSchemaToken if for some reason the schema dropped
    // them — we treat that as a config bug and fall back to invalid.
    const auto intLitKind   = schema_->schemaTokens().find("IntLiteral");
    const auto floatLitKind = schema_->schemaTokens().find("FloatLiteral");
    const auto eofKind      = schema_->schemaTokens().find("Eof");
    const auto errorKind    = schema_->schemaTokens().find("Error");

    while (!r.isAtEnd()) {
        const auto start = static_cast<ByteOffset>(r.position());
        const char c     = r.peek();

        // ── whitespace (one byte per token; matches per-char schema convention) ──
        if (isAsciiSpace(c) || c == '\n') {
            r.advance(1);
            const auto span = SourceSpan::of(start, static_cast<ByteOffset>(r.position()));
            const auto lexeme = r.slice(start, r.position());
            const auto hit = longestMatch(*schema_, lexeme);
            tokens.push_back(Token{
                .coreKind   = coreKindForByte(c),
                .schemaKind = hit.length > 0 ? hit.meaning.id : InvalidSchemaToken,
                .span       = span,
            });
            continue;
        }

        // ── identifier / keyword ──
        if (isIdStart(c)) {
            r.advance(1);
            while (isIdContinue(r.peek())) r.advance(1);
            const auto span = SourceSpan::of(start, static_cast<ByteOffset>(r.position()));
            const auto lexeme = r.slice(start, r.position());
            const auto hit = longestMatch(*schema_, lexeme);
            tokens.push_back(Token{
                .coreKind   = CoreTokenKind::Word,
                .schemaKind = hit.length == lexeme.size() ? hit.meaning.id : InvalidSchemaToken,
                .span       = span,
            });
            continue;
        }

        // ── numeric ──
        if (isDigit(c)) {
            bool isFloat = false;
            scanNumber(r, isFloat);
            const auto span = SourceSpan::of(start, static_cast<ByteOffset>(r.position()));
            tokens.push_back(Token{
                .coreKind   = isFloat ? CoreTokenKind::FloatLiteral : CoreTokenKind::IntLiteral,
                .schemaKind = isFloat ? floatLitKind : intLitKind,
                .span       = span,
            });
            continue;
        }

        // ── operator / punctuation longest-match ──
        const auto hit = longestMatch(*schema_, r.remaining());
        if (hit.length > 0) {
            r.advance(hit.length);
            const auto span = SourceSpan::of(start, static_cast<ByteOffset>(r.position()));
            tokens.push_back(Token{
                .coreKind   = coreKindForByte(c),
                .schemaKind = hit.meaning.id,
                .span       = span,
            });
            continue;
        }

        // ── illegal char ──
        //
        // Nothing matched; consume one byte and emit an Error token.
        // Tokenization continues so a single bad byte doesn't truncate
        // the rest of the stream.
        r.advance(1);
        const auto span = SourceSpan::of(start, static_cast<ByteOffset>(r.position()));
        tokens.push_back(Token{
            .coreKind   = CoreTokenKind::Error,
            .schemaKind = errorKind,
            .span       = span,
        });

        ParseDiagnostic d;
        d.code     = DiagnosticCode::P_IllegalChar;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = source_->id();
        d.span     = span;
        d.actual   = std::format("illegal character 0x{:02x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
        reporter_->report(std::move(d));
    }

    // ── trailing Eof ──
    //
    // Span is zero-width at end-of-buffer. TokenStream's contract
    // requires this final entry; peek() past it keeps returning the
    // same Eof so parsers don't need an isAtEnd() guard at every step.
    const auto eofSpan = SourceSpan::of(
        static_cast<ByteOffset>(r.size()),
        static_cast<ByteOffset>(r.size()));
    tokens.push_back(Token{
        .coreKind   = CoreTokenKind::Eof,
        .schemaKind = eofKind,
        .span       = eofSpan,
    });

    return { TokenStream{std::move(tokens), nextStreamInstanceId()},
             std::move(reporter_) };
}

} // namespace dss
