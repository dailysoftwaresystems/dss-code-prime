#include "tokenizer/tokenizer.hpp"

#include "core/types/lexer_mode.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/string_style.hpp"
#include "tokenizer/source_reader.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <regex>
#include <string>
#include <utility>
#include <vector>

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

// ── UTF-8 codepoint length ────────────────────────────────────────────────
//
// In a body mode (where the user chose per-codepoint defaultToken
// emission — see `.plans/tokenizer-plan.md` §2.3), the tokenizer needs
// to advance one codepoint at a time, not one byte. UTF-8 lead-byte
// ranges:
//   0x00-0x7F   single-byte ASCII (1)
//   0xC2-0xDF   2-byte lead       (2)
//   0xE0-0xEF   3-byte lead       (3)
//   0xF0-0xF4   4-byte lead       (4)
// Ill-formed leads (0x80-0xC1, 0xF5-0xFF) consume a single byte so the
// tokenizer makes progress; the illegal-char path picked them up before
// body-mode entry in well-behaved sources.
[[nodiscard]] constexpr std::size_t codepointByteCount(char leadByte) noexcept {
    const auto u = static_cast<unsigned char>(leadByte);
    if (u < 0x80) return 1;
    if (u < 0xC2) return 1;
    if (u < 0xE0) return 2;
    if (u < 0xF0) return 3;
    if (u < 0xF5) return 4;
    return 1;
}

// ── body-mode close-pattern matching ──────────────────────────────────────
//
// Check whether the current SourceReader position starts a body-mode
// close pattern. Returns the byte count to consume (0 on no match).
//
// `endsAt`        — the static suffix from the StringStyle (e.g. `'`,
//                   `*/`, `\n`).
// `dynamicSuffix` — the captured-at-opener tag for tagPattern-style
//                   raw strings (C++ `R"DELIM(...)DELIM"`). The full
//                   close pattern is `endsAt + dynamicSuffix`. Empty
//                   for static-endsAt configs (the common case).
// `longestMatch`  — when true, accept the longest run of `endsAt`'s
//                   single character (e.g. Lua `[==[ ... ]==]` where
//                   the closer count must match the opener's). When
//                   the dynamicSuffix is non-empty this flag is
//                   irrelevant (the dynamic tag IS the disambiguator).
[[nodiscard]] std::size_t matchEndsAt(SourceReader const& r,
                                      std::string_view endsAt,
                                      std::string_view dynamicSuffix,
                                      bool longestMatch) noexcept {
    const auto remaining = r.remaining();
    const std::size_t totalLen = endsAt.size() + dynamicSuffix.size();
    if (remaining.size() < totalLen) return 0;
    if (remaining.substr(0, endsAt.size()) != endsAt) return 0;
    if (!dynamicSuffix.empty()
        && remaining.substr(endsAt.size(), dynamicSuffix.size()) != dynamicSuffix) {
        return 0;
    }
    if (longestMatch && endsAt.size() == 1 && dynamicSuffix.empty()) {
        // Eat the longest run of the single ends-at character (Lua /
        // shell heredoc style). Caller's responsibility to declare a
        // single-character endsAt when this is on.
        std::size_t n = 0;
        while (n < remaining.size() && remaining[n] == endsAt[0]) ++n;
        return n;
    }
    return totalLen;
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

// Recognised numeric-suffix characters — used both as start- and
// continue-predicate (`42ull` walks four chars through this). `e`/`E`
// are excluded (those are exponent introducers handled separately).
// Other letters terminate the number — `123abc` becomes two tokens.
[[nodiscard]] constexpr bool isNumberSuffixChar(char c) noexcept {
    return c == 'u' || c == 'U' || c == 'l' || c == 'L'
        || c == 'f' || c == 'F' || c == 'd' || c == 'D';
}

// Result of `scanNumber`: float vs int + malformed signal. Malformed
// fires for base-prefixed literals that have a prefix but no actual
// digits — `0x_`, `0b__`, `0o`. The token is still emitted (so the
// source span is covered), but the caller emits P_MalformedNumber so
// the diagnostic surfaces.
struct NumberScan {
    bool isFloat   = false;
    bool malformed = false;
};

// Helper: consume a base-prefix body (digits + underscores). Returns
// true if at least one *digit* was seen (not just underscores).
template <typename DigitPred>
[[nodiscard]] bool consumeBaseBody(SourceReader& r, DigitPred isBaseDigit) noexcept {
    bool sawDigit = false;
    while (isBaseDigit(r.peek()) || r.peek() == '_') {
        if (isBaseDigit(r.peek())) sawDigit = true;
        r.advance(1);
    }
    return sawDigit;
}

[[nodiscard]] NumberScan scanNumber(SourceReader& r) noexcept {
    NumberScan out;

    // Base-prefixed integer literals: 0x / 0b / 0o. Consume only when
    // the *first* digit was `0` AND the next char is one of x/X/b/B/o/O
    // AND the char after that is a valid digit for the base OR an
    // underscore. Pure-letter tails like `0xy` stay tokenized as `0`
    // plus identifier `xy`. Pure-underscore tails like `0x_` consume
    // the body and set `malformed` so the caller emits a diagnostic.
    if (r.peek() == '0') {
        const char p1 = r.peek(1);
        const char p2 = r.peek(2);
        auto consumeSuffix = [&] {
            while (isNumberSuffixChar(r.peek())) r.advance(1);
        };
        if ((p1 == 'x' || p1 == 'X') && (isHexDigit(p2) || p2 == '_')) {
            r.advance(2);
            const bool sawDigit = consumeBaseBody(r, isHexDigit);
            consumeSuffix();
            out.malformed = !sawDigit;
            return out;
        }
        if ((p1 == 'b' || p1 == 'B') && (isBinDigit(p2) || p2 == '_')) {
            r.advance(2);
            const bool sawDigit = consumeBaseBody(r, isBinDigit);
            consumeSuffix();
            out.malformed = !sawDigit;
            return out;
        }
        if ((p1 == 'o' || p1 == 'O') && (isOctDigit(p2) || p2 == '_')) {
            r.advance(2);
            const bool sawDigit = consumeBaseBody(r, isOctDigit);
            consumeSuffix();
            out.malformed = !sawDigit;
            return out;
        }
    }

    // Decimal integer run.
    while (isDigit(r.peek())) r.advance(1);

    // Fractional part — `.` followed by a digit. Avoids gobbling `a.b`
    // member access (caller ensures we entered scanNumber on a digit;
    // the `.` is fractional only when more digits follow).
    if (r.peek() == '.' && isDigit(r.peek(1))) {
        out.isFloat = true;
        r.advance(1);                                  // '.'
        while (isDigit(r.peek())) r.advance(1);
    }

    // Exponent. `1e+5` / `1e-5` / `1e5` all valid; bare `1e` (no
    // digit) is not. When the exponent fails, mark the `e` as off-
    // limits to the suffix scan so it stays available as a fresh-
    // start identifier for the next tokenize iteration.
    bool sawEButNoExp = false;
    if (r.peek() == 'e' || r.peek() == 'E') {
        std::size_t look = 1;
        if (r.peek(1) == '+' || r.peek(1) == '-') look = 2;
        if (isDigit(r.peek(look))) {
            out.isFloat = true;
            r.advance(look + 1);
            while (isDigit(r.peek())) r.advance(1);
        } else {
            sawEButNoExp = true;
        }
    }

    // Suffix (restricted set — u/U/l/L promote nothing; f/F/d/D promote
    // an otherwise-int literal to float). Skipped when we just bailed
    // on a non-exponent `e`/`E`; the char belongs to the next token.
    if (!sawEButNoExp && isNumberSuffixChar(r.peek())) {
        const char first = r.peek();
        if (first == 'f' || first == 'F' || first == 'd' || first == 'D') {
            out.isFloat = true;
        }
        while (isNumberSuffixChar(r.peek())) r.advance(1);
    }

    return out;
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

TokenizeResult Tokenizer::tokenize() && {
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

    // ── lexer-mode stack ─────────────────────────────────────────────────
    //
    // Push the schema's "main" mode at start. Every schema synthesizes
    // "main" at load time (see PR5 loader), so `findLexerMode("main")`
    // always succeeds in well-formed configs. If for some reason the
    // schema dropped it we treat as fatal — body-mode lookups would
    // otherwise have nothing to fall back to.
    LexerModeStack modeStack;
    const auto mainModeId = schema_->findLexerMode("main");
    if (!mainModeId.valid()) tokenizerFatal("schema has no 'main' lexer mode");
    modeStack.push(mainModeId);

    // Parallel stacks for body-mode metadata. Indexed by mode-stack
    // depth - 1 (i.e. `styles[i]` is the StringStyle active at depth
    // i+1). The main mode has no style, so the stacks start empty.
    // `dynamicSuffix` holds the runtime-captured tag for stringStyles
    // with non-empty `tagPattern` (C++ raw strings, Rust r#"...#"); an
    // empty string means "static endsAt only."
    std::vector<StringStyle const*> styleStack;
    std::vector<std::string>        dynamicSuffix;

    // Locally-scoped emit helper — collapses the five "construct span +
    // push Token" sites that previously appeared once per branch. The
    // capture-by-reference includes the running `tokens` vector and the
    // `start` offset the loop body sets at each iteration's top.
    ByteOffset start = 0;
    auto emit = [&](CoreTokenKind ck, SchemaTokenId sk,
                    NodeFlags flagsApplied = NodeFlags::None) {
        (void)flagsApplied;   // reserved — Token has no flags field today
        tokens.push_back(Token{
            .coreKind   = ck,
            .schemaKind = sk,
            .span       = SourceSpan::of(start, static_cast<ByteOffset>(r.position())),
        });
    };

    // Apply a meaning's mode-stack side-effect after the corresponding
    // token has been emitted. `stringStyleId` is also threaded so a
    // pushMode-with-stringStyle correctly stacks the active style.
    auto applyMeaningSideEffects = [&](LexemeMeaning const& m) {
        switch (m.modeOp) {
            case ModeOp::None: break;
            case ModeOp::PushMode: {
                modeStack.push(m.modeArg);
                StringStyle const* style = schema_->stringStyle(m);
                styleStack.push_back(style);
                dynamicSuffix.emplace_back();
                // Dynamic tagPattern: capture at opener time so the
                // body-mode close pattern is `endsAt + capturedTag`.
                // Only fires when the style has a non-empty tagPattern.
                if (style && !style->tagPattern.empty()) {
                    try {
                        std::regex re(style->tagPattern);
                        std::cmatch match;
                        const auto remaining = r.remaining();
                        // `match_continuous` anchors at the start of
                        // remaining; greedy by default.
                        if (std::regex_search(remaining.data(),
                                              remaining.data() + remaining.size(),
                                              match, re,
                                              std::regex_constants::match_continuous)) {
                            dynamicSuffix.back().assign(match[0].first, match[0].second);
                            r.advance(static_cast<std::size_t>(match[0].length()));
                        }
                    } catch (std::exception const&) {
                        // tagPattern is loader-validated; a runtime
                        // regex failure here is a deep config bug. Keep
                        // tokenizing without a dynamic tag rather than
                        // crash.
                    }
                }
                break;
            }
            case ModeOp::PopMode: {
                if (!modeStack.tryPop()) break;
                if (!styleStack.empty()) styleStack.pop_back();
                if (!dynamicSuffix.empty()) dynamicSuffix.pop_back();
                break;
            }
            case ModeOp::ReplaceMode:
                if (modeStack.depth() > 0) modeStack.replaceTop(m.modeArg);
                if (!styleStack.empty()) {
                    styleStack.back() = schema_->stringStyle(m);
                    if (!dynamicSuffix.empty()) dynamicSuffix.back().clear();
                }
                break;
        }
    };

    // UTF-8 BOM at the start of the source: skip silently. Some
    // editors / git templates prepend it; treating it as an identifier
    // (`isIdStart` previously accepted any byte ≥ 0x80) would leak
    // three garbage bytes into the first identifier of the file.
    //
    // A stray BOM AFTER position 0 cannot be distinguished from a
    // legitimate U+FEFF zero-width-no-break-space codepoint — both
    // are the byte sequence `EF BB BF`. Per the byte-pass-through
    // identifier model, mid-source BOM bytes get absorbed by any
    // surrounding identifier scan (i.e. `var<BOM>x` is one Word
    // token, just like a deliberately-pasted U+FEFF would be). The
    // silent-failure-hunter review surfaced this trade-off; the
    // resolution is to accept it as a v3-class concern. Full Unicode
    // identifier semantics (XID_Start / XID_Continue) would let us
    // distinguish "control-class" from "letter-class" codepoints and
    // properly reject stray BOM bytes.
    if (r.size() >= 3
        && static_cast<unsigned char>(r.peek(0)) == 0xEF
        && static_cast<unsigned char>(r.peek(1)) == 0xBB
        && static_cast<unsigned char>(r.peek(2)) == 0xBF) {
        r.advance(3);
    }

    while (!r.isAtEnd()) {
        start = static_cast<ByteOffset>(r.position());

        // ── body-mode branch ────────────────────────────────────────
        //
        // When the current mode declares a `defaultToken`, we're inside
        // a string body, a comment, or any other "consume until endsAt"
        // construct. The mode's per-token table is empty (per PR5's
        // design); resolution is: try endsAt → try escape → emit one
        // codepoint as defaultToken.
        const auto& currentMode = schema_->lexerMode(modeStack.top());
        if (currentMode.defaultToken.has_value()) {
            StringStyle const* style = styleStack.empty()
                ? nullptr
                : styleStack.back();
            std::string_view suffix = dynamicSuffix.empty()
                ? std::string_view{}
                : std::string_view{dynamicSuffix.back()};

            if (style) {
                // 1. Doubled-delimiter escape: `''` inside a SQL
                //    string is a literal `'`, NOT the end of the
                //    body. Check before plain endsAt match so the
                //    longer match wins.
                if (style->escapeKind == EscapeKind::DoubledDelimiter
                    && r.remaining().size() >= 2 * style->endsAt.size()
                    && r.remaining().substr(0, style->endsAt.size()) == style->endsAt
                    && r.remaining().substr(style->endsAt.size(), style->endsAt.size()) == style->endsAt) {
                    r.advance(2 * style->endsAt.size());
                    emit(CoreTokenKind::Operator, *currentMode.defaultToken);
                    continue;
                }

                // 2. Static-or-dynamic endsAt match: consume + pop mode.
                if (const auto n = matchEndsAt(r, style->endsAt, suffix,
                                               style->endsAtLongestMatch);
                    n > 0) {
                    r.advance(n);
                    emit(CoreTokenKind::Operator, *currentMode.defaultToken);
                    if (!modeStack.tryPop()) tokenizerFatal("mode stack underflow at endsAt");
                    if (!styleStack.empty()) styleStack.pop_back();
                    if (!dynamicSuffix.empty()) dynamicSuffix.pop_back();
                    continue;
                }

                // 3. Char-escape: lead byte + next codepoint emitted
                //    as one defaultToken. Permissive — any byte after
                //    the lead is accepted (P_InvalidEscape only fires
                //    when the lead is the last byte of the source).
                if (style->escapeKind == EscapeKind::Char
                    && static_cast<unsigned char>(r.peek()) == static_cast<unsigned char>(style->escapeChar)) {
                    r.advance(1);
                    if (r.isAtEnd()) {
                        ParseDiagnostic d;
                        d.code     = DiagnosticCode::P_InvalidEscape;
                        d.severity = DiagnosticSeverity::Error;
                        d.buffer   = source_->id();
                        d.span     = SourceSpan::of(start, static_cast<ByteOffset>(r.position()));
                        d.actual   = "escape lead at end of source";
                        reporter_->report(std::move(d));
                    } else {
                        r.advance(codepointByteCount(r.peek()));
                    }
                    emit(CoreTokenKind::Operator, *currentMode.defaultToken);
                    continue;
                }
            }

            // 4. Default: consume one codepoint as the body's default
            //    token kind. UTF-8 multi-byte sequences land in one
            //    token (per user-chosen per-codepoint granularity).
            r.advance(codepointByteCount(r.peek()));
            emit(CoreTokenKind::Operator, *currentMode.defaultToken);
            continue;
        }

        const char c = r.peek();

        // Whitespace runs emit one token per byte (matches the per-char
        // schema convention) — the pre-resolved kinds avoid the
        // longest-match loop for these known single-byte lookups.
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

        // Identifier or keyword: alphanumeric/underscore run; the
        // schema lookup decides if the resulting lexeme is a keyword
        // (schemaKind valid) or a plain identifier (schemaKind invalid;
        // builder's pushToken fallback promotes to Identifier).
        if (isIdStart(c)) {
            r.advance(1);
            while (isIdContinue(r.peek())) r.advance(1);
            const auto lexeme = r.slice(start, r.position());
            const auto hit = longestMatch(*schema_, lexeme, lexemeProbeMax);
            // Only honor the lookup when it covers the entire run; a
            // schema entry like `int` shouldn't claim part of `integer`.
            const bool fullMatch = (hit.length == lexeme.size());
            const auto sk = fullMatch ? hit.meaning.id : InvalidSchemaToken;
            emit(CoreTokenKind::Word, sk);
            if (fullMatch) applyMeaningSideEffects(hit.meaning);
            continue;
        }

        // Numeric literals — hand-coded; see `scanNumber` for the
        // grammar. Int vs Float is decided inside `scanNumber` based
        // on whether `.`/`e`/`E` or a float-marker suffix was consumed.
        // Malformed base-prefix literals (`0x_` with no actual digit)
        // still get a token emitted but also produce P_MalformedNumber.
        if (isDigit(c)) {
            const auto scan = scanNumber(r);
            emit(scan.isFloat ? CoreTokenKind::FloatLiteral : CoreTokenKind::IntLiteral,
                 scan.isFloat ? floatLitKind : intLitKind);
            if (scan.malformed) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::P_MalformedNumber;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = source_->id();
                d.span     = SourceSpan::of(start, static_cast<ByteOffset>(r.position()));
                d.actual   = std::format("'{}'", r.slice(start, r.position()));
                reporter_->report(std::move(d));
            }
            continue;
        }

        // Operator or punctuation: longest-match against the schema's
        // declared lexeme keys, bounded by the schema's longest key
        // length (no silent truncation on a future 5+ char lexeme).
        const auto hit = longestMatch(*schema_, r.remaining(), lexemeProbeMax);
        if (hit.length > 0) {
            r.advance(hit.length);
            emit(coreKindForByte(c), hit.meaning.id);
            applyMeaningSideEffects(hit.meaning);
            continue;
        }

        // Illegal char fallback: nothing matched; consume one byte
        // and emit an Error token plus a diagnostic.
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

    // Unterminated body mode: source ended before the active body's
    // endsAt was matched. Emit one diagnostic per unterminated mode
    // (excluding the always-on "main"). Diagnostic flavor picks string
    // vs comment heuristically off the mode name — a config can choose
    // whichever is most useful for its IDE renderer.
    while (modeStack.depth() > 1) {
        const auto& mode = schema_->lexerMode(modeStack.top());
        const bool looksLikeComment = mode.name.find("comment") != std::string::npos;
        ParseDiagnostic d;
        d.code     = looksLikeComment
            ? DiagnosticCode::P_UnterminatedComment
            : DiagnosticCode::P_UnterminatedString;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = source_->id();
        d.span     = SourceSpan::of(static_cast<ByteOffset>(r.size()),
                                    static_cast<ByteOffset>(r.size()));
        d.actual   = std::format("EOF inside lexer mode '{}'", mode.name);
        reporter_->report(std::move(d));
        modeStack.tryPop();
        if (!styleStack.empty()) styleStack.pop_back();
        if (!dynamicSuffix.empty()) dynamicSuffix.pop_back();
    }

    // Trailing Eof: span is zero-width at end-of-buffer. TokenStream's contract
    // requires this final entry; peek() past it keeps returning the
    // same Eof so parsers don't need an isAtEnd() guard at every step.
    start = static_cast<ByteOffset>(r.size());
    emit(CoreTokenKind::Eof, eofKind);

    return TokenizeResult{
        .stream      = TokenStream{std::move(tokens), nextStreamInstanceId()},
        .diagnostics = std::move(reporter_),
    };
}

} // namespace dss
