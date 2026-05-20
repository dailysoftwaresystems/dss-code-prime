#include "tokenizer/tokenizer.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "tokenizer/source_reader.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <utility>

namespace dss {

namespace {

// Layer-local fatal helper. Same posture as treeFatal / attrFatal /
// streamFatal: always-on, release-mode abort, prefix identifies the
// originating layer for triage. SKILL.md mandates this pattern over
// `<cassert>` (the latter is debug-only and silenced in Release).
[[noreturn]] void tokenizerFatal(char const* what) {
    std::fputs("dss::Tokenizer fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

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

// UTF-8 leading bytes are 0xC2..0xF4. Bytes 0x80..0xBF are continuation
// bytes — only valid as the tail of a multi-byte sequence; appearing
// at token start signals malformed UTF-8 and lands in the illegal-char
// path. Bytes 0xC0..0xC1 and 0xF5..0xFF are reserved and never appear
// in well-formed UTF-8. The byte-level isIdContinue accepts the full
// 0x80..0xFF range so multi-byte continuation runs (already started
// by a valid lead byte) don't terminate the identifier.
[[nodiscard]] constexpr bool isIdStart(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_'
        || (u >= 0xC2 && u <= 0xF4);
}

[[nodiscard]] constexpr bool isIdContinue(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_'
        || (u >= '0' && u <= '9')
        || u >= 0x80;   // accept any non-ASCII byte (continuation bytes legal mid-run)
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
// Probe length is bounded by the schema's longest declared lexeme key
// (`GrammarSchema::maxLexemeLength`). A schema author who adds a 5-char
// lexeme later doesn't silently see it truncated — the tokenizer
// expands its window automatically. Floor of 1 keeps the loop sane on
// schemas with no `tokens` declarations.

struct LookupHit {
    std::size_t      length = 0;       // bytes consumed; 0 = no match
    LexemeMeaning    meaning{};        // the winning candidate
};

[[nodiscard]] LookupHit longestMatch(GrammarSchema const& schema,
                                     std::string_view remaining,
                                     std::size_t maxLength) noexcept {
    if (maxLength == 0) return {};
    const std::size_t maxN = std::min(maxLength, remaining.size());
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
//   prefix-int := 0 (x|X) [0-9a-fA-F_]+ suffix?   // hex
//              |  0 (b|B) [01_]+ suffix?           // binary
//              |  0 (o|O) [0-7_]+ suffix?           // octal
//   int        := [0-9]+ suffix?                   // decimal
//   float      := ([0-9]+ \. [0-9]*  |  [0-9]+ exponent) suffix?
//   exponent   := (e|E) [+-]? [0-9]+
//   suffix     := [uUlL]+ | f | F | d | D | (combinations like ul, ull)
//
// Three classes of bug this scan now avoids:
//
//   1. `0xff` was previously consumed as `0` + suffix `xff` — the
//      base-prefix branch below resolves it as a hex IntLiteral.
//   2. `1ex` (bare exponent + identifier) — the exponent backoff
//      (`sawEButNoExp`) leaves `e` for the next iteration.
//   3. `123abc` — the suffix loop now only accepts letters from the
//      type-marker set (u/U/l/L/f/F/d/D), so `123abc` becomes
//      `123` + `abc` rather than one bogus IntLiteral.
//
// Languages whose numeric grammar deviates from this C-style scheme
// will eventually need a `numberStyle` schema descriptor (v2-gap-
// catalog row 14). Until then, every shipped config inherits the
// rules above as the universal default.

[[nodiscard]] constexpr bool isHexDigit(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
}

[[nodiscard]] constexpr bool isBinDigit(char c) noexcept { return c == '0' || c == '1'; }

[[nodiscard]] constexpr bool isOctDigit(char c) noexcept {
    return c >= '0' && c <= '7';
}

// Recognised numeric-suffix START characters. `e`/`E` are excluded
// (those are exponent introducers handled separately). Other letters
// terminate the number — a `123abc` slug becomes two tokens, not one.
[[nodiscard]] constexpr bool isNumberSuffixStart(char c) noexcept {
    return c == 'u' || c == 'U' || c == 'l' || c == 'L'
        || c == 'f' || c == 'F' || c == 'd' || c == 'D';
}

void scanNumber(SourceReader& r, bool& outIsFloat) noexcept {
    outIsFloat = false;

    // Base-prefixed integer literals: 0x / 0b / 0o. Consume only when
    // the *first* digit was `0` AND the next char is one of x/X/b/B/o/O
    // AND the char after that is a valid digit for the base. The
    // last condition keeps `0xy` (the var `xy` after a literal `0`)
    // tokenized as `0` + `xy`.
    if (r.peek() == '0') {
        const char p1 = r.peek(1);
        const char p2 = r.peek(2);
        if ((p1 == 'x' || p1 == 'X') && (isHexDigit(p2) || p2 == '_')) {
            r.advance(2);
            while (isHexDigit(r.peek()) || r.peek() == '_') r.advance(1);
            if (isNumberSuffixStart(r.peek())) {
                while (isNumberSuffixStart(r.peek())) r.advance(1);
            }
            return;   // hex never floats
        }
        if ((p1 == 'b' || p1 == 'B') && (isBinDigit(p2) || p2 == '_')) {
            r.advance(2);
            while (isBinDigit(r.peek()) || r.peek() == '_') r.advance(1);
            if (isNumberSuffixStart(r.peek())) {
                while (isNumberSuffixStart(r.peek())) r.advance(1);
            }
            return;
        }
        if ((p1 == 'o' || p1 == 'O') && (isOctDigit(p2) || p2 == '_')) {
            r.advance(2);
            while (isOctDigit(r.peek()) || r.peek() == '_') r.advance(1);
            if (isNumberSuffixStart(r.peek())) {
                while (isNumberSuffixStart(r.peek())) r.advance(1);
            }
            return;
        }
    }

    // Decimal integer run.
    while (isDigit(r.peek())) r.advance(1);

    // Fractional part — `.` followed by a digit. Avoids gobbling `a.b`
    // member access (caller ensures we entered scanNumber on a digit;
    // the `.` is fractional only when more digits follow).
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

    // Suffix (restricted set — u/U/l/L promote nothing; f/F/d/D promote
    // an otherwise-int literal to float). Skipped when we just bailed
    // on a non-exponent `e`/`E`; the char belongs to the next token.
    if (!sawEButNoExp && isNumberSuffixStart(r.peek())) {
        const char first = r.peek();
        if (first == 'f' || first == 'F' || first == 'd' || first == 'D') {
            outIsFloat = true;
        }
        while (isNumberSuffixStart(r.peek())) r.advance(1);
    }
}

} // namespace

Tokenizer::Tokenizer(std::shared_ptr<SourceBuffer>        src,
                     std::shared_ptr<GrammarSchema const> schema,
                     DiagnosticReporter::Config           diagConfig)
    : source_(std::move(src))
    , schema_(std::move(schema))
    , reporter_(std::make_unique<DiagnosticReporter>(std::move(diagConfig))) {
    if (!source_) tokenizerFatal("source is null");
    if (!schema_) tokenizerFatal("schema is null");
}

std::pair<TokenStream, std::unique_ptr<DiagnosticReporter>>
Tokenizer::tokenize() && {
    SourceReader r{*source_};
    std::vector<Token> tokens;

    // Pre-resolve every schema-token kind the tokenizer might emit
    // directly. The schema's SchemaTokenInterner pre-interns the
    // built-ins at load time; `find` returns the existing id or
    // `InvalidSchemaToken` if for some reason the schema dropped them.
    // Caching the whitespace kinds is a substantive win — without it,
    // each whitespace byte runs the longest-match loop for a known
    // single-byte lookup.
    const auto intLitKind   = schema_->schemaTokens().find("IntLiteral");
    const auto floatLitKind = schema_->schemaTokens().find("FloatLiteral");
    const auto eofKind      = schema_->schemaTokens().find("Eof");
    const auto errorKind    = schema_->schemaTokens().find("Error");
    const auto wsKind       = schema_->schemaTokens().find("Whitespace");
    const auto newlineKind  = schema_->schemaTokens().find("Newline");

    // Probe length is bounded by the schema's longest declared lexeme
    // key. Recomputing this per call would re-walk the lexeme table;
    // pin it once for the entire tokenize() invocation.
    const std::size_t lexemeProbeMax = schema_->maxLexemeLength();

    // Locally-scoped emit helper — collapses the five "construct span +
    // push Token" sites that previously appeared once per branch. The
    // capture-by-reference includes the running `tokens` vector and the
    // `start` offset the loop body sets at each iteration's top.
    ByteOffset start = 0;
    auto emit = [&](CoreTokenKind ck, SchemaTokenId sk) {
        tokens.push_back(Token{
            .coreKind   = ck,
            .schemaKind = sk,
            .span       = SourceSpan::of(start, static_cast<ByteOffset>(r.position())),
        });
    };

    // UTF-8 BOM at the start of the source: skip silently. Some
    // editors / git templates prepend it; treating it as an identifier
    // (`isIdStart` previously accepted any byte ≥ 0x80) leaks three
    // garbage bytes into the first identifier of the file.
    if (r.size() >= 3
        && static_cast<unsigned char>(r.peek(0)) == 0xEF
        && static_cast<unsigned char>(r.peek(1)) == 0xBB
        && static_cast<unsigned char>(r.peek(2)) == 0xBF) {
        r.advance(3);
    }

    while (!r.isAtEnd()) {
        start = static_cast<ByteOffset>(r.position());
        const char c = r.peek();

        // ── whitespace (one byte per token; matches per-char schema convention) ──
        if (c == '\n') {
            r.advance(1);
            emit(CoreTokenKind::Newline, newlineKind);
            continue;
        }
        if (isAsciiSpace(c)) {
            r.advance(1);
            emit(CoreTokenKind::Whitespace, wsKind);
            continue;
        }

        // ── identifier / keyword ──
        if (isIdStart(c)) {
            r.advance(1);
            while (isIdContinue(r.peek())) r.advance(1);
            const auto lexeme = r.slice(start, r.position());
            const auto hit = longestMatch(*schema_, lexeme, lexemeProbeMax);
            // Only honor the lookup when it covers the entire run; a
            // schema entry like `int` shouldn't claim part of `integer`.
            const auto sk = (hit.length == lexeme.size()) ? hit.meaning.id : InvalidSchemaToken;
            emit(CoreTokenKind::Word, sk);
            continue;
        }

        // ── numeric ──
        if (isDigit(c)) {
            bool isFloat = false;
            scanNumber(r, isFloat);
            emit(isFloat ? CoreTokenKind::FloatLiteral : CoreTokenKind::IntLiteral,
                 isFloat ? floatLitKind : intLitKind);
            continue;
        }

        // ── operator / punctuation longest-match ──
        const auto hit = longestMatch(*schema_, r.remaining(), lexemeProbeMax);
        if (hit.length > 0) {
            r.advance(hit.length);
            emit(coreKindForByte(c), hit.meaning.id);
            continue;
        }

        // ── illegal char ──
        //
        // Nothing matched; consume one byte and emit an Error token.
        // Tokenization continues so a single bad byte doesn't truncate
        // the rest of the stream. Bare UTF-8 continuation bytes
        // (0x80-0xBF — only valid as the tail of a multi-byte sequence)
        // also land here because `isIdStart` rejects them.
        r.advance(1);
        emit(CoreTokenKind::Error, errorKind);

        ParseDiagnostic d;
        d.code     = DiagnosticCode::P_IllegalChar;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = source_->id();
        d.span     = SourceSpan::of(start, static_cast<ByteOffset>(r.position()));
        d.actual   = std::format("illegal character 0x{:02x}",
                                 static_cast<unsigned>(static_cast<unsigned char>(c)));
        reporter_->report(std::move(d));
    }

    // ── trailing Eof ──
    //
    // Span is zero-width at end-of-buffer. TokenStream's contract
    // requires this final entry; peek() past it keeps returning the
    // same Eof so parsers don't need an isAtEnd() guard at every step.
    start = static_cast<ByteOffset>(r.size());
    emit(CoreTokenKind::Eof, eofKind);

    return { TokenStream{std::move(tokens), nextStreamInstanceId()},
             std::move(reporter_) };
}

} // namespace dss
