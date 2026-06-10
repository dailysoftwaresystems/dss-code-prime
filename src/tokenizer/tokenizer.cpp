#include "tokenizer/tokenizer.hpp"

#include "core/types/lexer_mode.hpp"
#include "core/types/number_style.hpp"
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

// ── per-mode (context-sensitive) lexeme lookup ───────────────────────────
//
// Mode-aware longest-match. The active lexer mode may declare a per-mode
// `tokens` override table (parsed by the loader into
// `lexerModeTokens[mode]`); while in that mode the scanner consults the
// override table FIRST. The override wins wherever it matches — even at a
// shorter length than a competing GLOBAL lexeme — because a
// context-sensitive mode exists precisely to redefine what a character
// means in that context (e.g. `<` opening a header path inside an
// `#include` mode while staying `LtOp` everywhere else).
//
// ZERO-REGRESSION FALLBACK (the load-bearing correctness property): if the
// mode's override table produces NO match, the result is the EXACT global
// `longestMatch` — the same code path the tokenizer used before per-mode
// lexing existed. In the default mode that every shipped language uses
// (its override table is a verbatim copy of the global `lexemeTable`, and
// no shipped grammar declares a distinct override), the override-table
// longest-match equals the global longest-match for every input, so this
// function is byte-identical to the prior behavior. A grammar that does
// declare overrides only diverges for the lexemes it overrides, while in
// that mode; everything else still routes through the global fallback.
[[nodiscard]] LookupHit longestMatchInMode(GrammarSchema const& schema,
                                           LexerModeId mode,
                                           std::string_view remaining,
                                           std::size_t maxLength) noexcept {
    if (maxLength == 0) return {};
    const std::size_t maxN = std::min(maxLength, remaining.size());
    // 1. Override table first — longest declared override key wins.
    for (std::size_t len = maxN; len >= 1; --len) {
        const auto cands = schema.lookupLexemeInMode(mode, remaining.substr(0, len));
        if (cands.empty()) continue;
        return LookupHit{ .length = len, .meaning = cands[0] };
    }
    // 2. Nothing in the mode overrides this position — fall back to the
    //    exact global longest-match path (unchanged behavior).
    return longestMatch(schema, remaining, maxLength);
}

// ── UTF-8 codepoint length ────────────────────────────────────────────────
//
// In a body mode (where the user chose per-codepoint defaultToken
// emission — see `.plans/04-tokenizer-plan - ok.md` §2.3), the tokenizer needs
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
// `longestMatch`  — when true AND `endsAt.size() == 1` AND no
//                   dynamicSuffix, eat the entire run of consecutive
//                   `endsAt` bytes as one close emission (Lua / shell
//                   heredoc style). Note: this does NOT enforce that
//                   the closer's run length matches the opener's —
//                   any non-empty run terminates. When `endsAt` is
//                   multi-char OR dynamicSuffix is non-empty the flag
//                   is silently a no-op (exact-length match runs).
//                   The loader rejects the longestMatch + tagPattern
//                   combo at load time.
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
// Universally config-driven (08.55 cleanup; schema v4 `numberStyle`).
// The tokenizer's numeric scanner reads the active language's
// `GrammarSchema::numberStyle()` and walks input accordingly — no
// hardcoded letter classes, no baked C-style assumptions. A language
// with no numeric literals omits the block entirely; the loader emits
// `C_MissingNumberStyle` when `IntLiteral`/`FloatLiteral` are
// referenced from shapes without a block.
//
// Diagnostic preservation:
//   - `P_MalformedNumber` still fires when a prefix is matched but no
//     real digit (only separators) follows — `0x_`, `0b__`.
//   - `P_IllegalChar` is owned by the dispatch loop (unchanged).

// Test whether `c` lands in the digit character-class string. The
// class syntax supports literal chars and `a-z` ranges (the same shape
// every shipped config uses); unknown forms are interpreted literally.
[[nodiscard]] bool matchesDigitClass(std::string_view digits, char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    for (std::size_t i = 0; i < digits.size(); ++i) {
        // `a-z` range form.
        if (i + 2 < digits.size() && digits[i + 1] == '-') {
            const auto lo = static_cast<unsigned char>(digits[i]);
            const auto hi = static_cast<unsigned char>(digits[i + 2]);
            if (u >= lo && u <= hi) return true;
            i += 2;
            continue;
        }
        if (static_cast<unsigned char>(digits[i]) == u) return true;
    }
    return false;
}

[[nodiscard]] bool isSeparator(NumberStyle const& s, char c) noexcept {
    return s.digitSeparator.has_value() && *s.digitSeparator == c;
}

// Result of `scanNumber`: float vs int + malformed signal. Malformed
// fires for prefix literals that have a prefix but no actual digits —
// `0x_`, `0b__`. The token is still emitted (so the source span is
// covered), but the caller emits P_MalformedNumber.
struct NumberScan {
    bool isFloat   = false;
    bool malformed = false;
};

// Longest-match against a list of suffixes at the reader's current
// position. Returns 0 when nothing matched (the suffix tail is
// optional in every shipped config). Suffix strings are compared
// byte-for-byte against the upcoming bytes.
[[nodiscard]] std::size_t matchLongestSuffix(SourceReader const& r,
                                             std::vector<std::string> const& suffixes) noexcept {
    std::size_t best = 0;
    const auto rem = r.remaining();
    for (auto const& s : suffixes) {
        if (s.size() > rem.size()) continue;
        if (s.size() <= best) continue;
        if (rem.substr(0, s.size()) == s) best = s.size();
    }
    return best;
}

[[nodiscard]] NumberScan scanNumber(SourceReader& r, NumberStyle const& style) noexcept {
    NumberScan out;

    // 1. Try integer prefixes in declaration order. The loader's
    //    ordering is preserved so a config can disambiguate
    //    overlapping prefixes by listing the longer one first
    //    (e.g. `0x` before `0`). Per-prefix digit class drives
    //    body acceptance; separators are accepted as long as the
    //    schema declares one.
    for (auto const& p : style.integerPrefixes) {
        const auto rem = r.remaining();
        if (rem.size() < p.prefix.size()) continue;
        if (rem.substr(0, p.prefix.size()) != p.prefix) continue;
        // Lookahead resolution after a prefix match:
        //   - EOF after a MULTI-CHAR prefix: the prefix is a designator
        //     (e.g. `0x`, `0b`, `0o`) and the body is missing. Consume
        //     the prefix as a malformed IntLiteral so the diagnostic
        //     surfaces. Splitting `0x` into `0` + `x` would silently
        //     swallow the user's typo.
        //   - EOF after a SINGLE-CHAR prefix (e.g. C-style bare `0`
        //     octal): the prefix byte is ITSELF a valid digit in the
        //     prefix's own digit class, so `0` at EOF is a valid zero,
        //     not malformed. Fall through to the bare-decimal arm
        //     (which consumes the byte and emits IntLiteral cleanly).
        //   - Non-digit, non-separator follow-up: leave the prefix
        //     alone so `0xy` keeps parsing as `0` + identifier `xy`
        //     (a shorter prefix or the bare-decimal arm picks the `0`).
        const bool atEofAfterPrefix = p.prefix.size() >= rem.size();
        const char afterPrefix =
            !atEofAfterPrefix ? rem[p.prefix.size()] : '\0';
        if (atEofAfterPrefix && p.prefix.size() == 1) {
            // Single-char prefix at EOF: not malformed; let a later
            // pass handle it (bare-decimal arm if decimal is enabled,
            // longest-match otherwise).
            continue;
        }
        if (!atEofAfterPrefix
            && !matchesDigitClass(p.digits, afterPrefix)
            && !isSeparator(style, afterPrefix)) {
            continue;
        }
        r.advance(p.prefix.size());
        bool sawDigit = false;
        while (true) {
            const char c = r.peek();
            if (matchesDigitClass(p.digits, c)) { sawDigit = true; r.advance(1); continue; }
            // FC1 (V2-4.X, 2026-06-10): a digit separator is consumed
            // only BETWEEN digits (C23 6.4.4.1 — each separator must
            // be flanked by digits). The previous unconditional
            // consume silently swallowed trailing/leading separators
            // (`1'` lexed as the value 1, eating the quote) — fatal
            // once a language's separator collides with another token
            // start (C23's `'` is also the char-literal quote: `1'+'a'`
            // must NOT lex as 1 then garbage). Universal rule, no
            // language identity: any schema-declared separator gets
            // the flanked-by-digits requirement.
            if (sawDigit && isSeparator(style, c)
                && matchesDigitClass(p.digits, r.peek(1))) {
                r.advance(1);
                continue;
            }
            break;
        }
        // Optional integer suffix.
        if (const auto n = matchLongestSuffix(r, style.integerSuffixes); n > 0) {
            r.advance(n);
        }
        out.malformed = !sawDigit;
        return out;
    }

    // 2. Decimal body — only when the schema opts into bare decimals.
    //    The dispatch loop ensures we entered on an isDigit byte; if
    //    the schema sets `decimal: false`, we still consume the bare
    //    digit run to make progress (caller treats it as malformed).
    bool sawDigit = false;
    auto consumeDecimalRun = [&] {
        while (true) {
            const char c = r.peek();
            if (c >= '0' && c <= '9') { sawDigit = true; r.advance(1); continue; }
            // Between-digits separator rule — see the prefix-body loop
            // above (C23 6.4.4.1 flanked-by-digits; universal for any
            // schema-declared separator).
            if (sawDigit && isSeparator(style, c)
                && r.peek(1) >= '0' && r.peek(1) <= '9') {
                r.advance(1);
                continue;
            }
            break;
        }
    };
    consumeDecimalRun();

    // 3. Fractional part — when fractionPoint is set AND followed by a
    //    digit (`a.b` member access doesn't gobble the dot).
    if (style.fractionPoint.has_value()
        && r.peek() == *style.fractionPoint
        && (r.peek(1) >= '0' && r.peek(1) <= '9')) {
        out.isFloat = true;
        r.advance(1);            // fraction point
        consumeDecimalRun();
    }

    // 4. Exponent — when declared. A failed exponent (letter without
    //    digits following) leaves the letter unconsumed so the next
    //    tokenize iteration sees it as a fresh start.
    bool sawExpLetterButNoExp = false;
    if (style.exponent.has_value()) {
        auto const& exp = *style.exponent;
        bool letterMatched = false;
        for (auto l : exp.letters) {
            if (r.peek() == l) { letterMatched = true; break; }
        }
        if (letterMatched) {
            std::size_t look = 1;
            if (exp.signOptional && (r.peek(1) == '+' || r.peek(1) == '-')) look = 2;
            const char afterSign = r.peek(look);
            if (afterSign >= '0' && afterSign <= '9') {
                out.isFloat = true;
                r.advance(look);  // letter (+ optional sign)
                consumeDecimalRun();
            } else {
                sawExpLetterButNoExp = true;
            }
        }
    }

    // 5. Suffixes — try float suffixes first (longer typically); a
    //    float-suffix match promotes to float kind. Skip when we
    //    bailed on a no-digit exponent letter (it belongs to the
    //    next token). Two-pass longest-match across both lists keeps
    //    selection deterministic; integer suffixes apply if no float
    //    suffix matched.
    if (!sawExpLetterButNoExp) {
        if (const auto fn = matchLongestSuffix(r, style.floatSuffixes); fn > 0) {
            out.isFloat = true;
            r.advance(fn);
        } else if (const auto in = matchLongestSuffix(r, style.integerSuffixes); in > 0) {
            r.advance(in);
        }
    }

    // Bare decimal with no digit body landed only on a separator —
    // surface as malformed. Reachable when the dispatch loop entered
    // on a digit char so we always saw at least one digit; the guard
    // is here for symmetry with the prefix branch.
    if (!sawDigit) out.malformed = true;

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
    const auto eofKind      = schema_->schemaTokens().find("Eof");
    const auto errorKind    = schema_->schemaTokens().find("Error");
    const auto wsKind       = schema_->schemaTokens().find("Whitespace");
    const auto newlineKind  = schema_->schemaTokens().find("Newline");

    // Numeric-literal grammar (08.55; config-driven). nullptr when the
    // language declares no `numberStyle`. The digit branch in the
    // dispatch loop falls back to longest-match when this is null so a
    // bare digit can still be claimed by a language-specific token.
    NumberStyle const* const numberStyle = schema_->numberStyle();

    // Probe length is bounded by the schema's longest declared lexeme
    // key. Recomputing this per call would re-walk the lexeme table;
    // pin it once for the entire tokenize() invocation.
    const std::size_t lexemeProbeMax = schema_->maxLexemeLength();

    // One stack of `Frame { mode, style, dynamicSuffix }` rather than
    // three parallel vectors. Atomic push/pop/replace closes the
    // desync class of bug surfaced in TZ2 review (defensive
    // `if (!styleStack.empty())` guards used to mask drift between the
    // three vectors). Every site now goes through frames.push_back /
    // pop_back / `frames.back() = …` — defensive guards become asserts.
    struct Frame {
        LexerModeId        mode;
        StringStyle const* style;        // null when mode has no opener-stringStyle
        std::string        dynamicSuffix;
    };
    std::vector<Frame> frames;

    const auto mainModeId = schema_->findLexerMode("main");
    if (!mainModeId.valid()) tokenizerFatal("schema has no 'main' lexer mode");
    frames.push_back(Frame{ .mode = mainModeId, .style = nullptr, .dynamicSuffix = {} });

    // Locally-scoped emit helper — collapses the five "construct span +
    // push Token" sites that previously appeared once per branch. The
    // capture-by-reference includes the running `tokens` vector and the
    // `start` offset the loop body sets at each iteration's top.
    //
    // `flagsApplied` lets a body-mode emission propagate the mode's
    // `defaultToken.flags` onto the resulting Token. The builder
    // OR-merges with the schema meaning's `flagsApplied` at pushToken
    // time so both sources of flag intent reach the AST.
    ByteOffset start = 0;
    auto emit = [&](CoreTokenKind ck, SchemaTokenId sk,
                    NodeFlags flagsApplied = NodeFlags::None) {
        tokens.push_back(Token{
            .coreKind   = ck,
            .flags      = flagsApplied,
            .schemaKind = sk,
            .span       = SourceSpan::of(start, static_cast<ByteOffset>(r.position())),
        });
    };

    // Tokenizer-local cache of compiled tagPattern regexes, keyed by
    // StringStyleId.v. `std::regex` construction is expensive and the
    // schema's tagPattern is constant per StringStyle for the
    // tokenizer's lifetime — compiling once per opener was a real perf
    // cliff for raw-string-heavy sources. Lazily populated on first use.
    std::unordered_map<std::uint32_t, std::regex> tagRegexCache;

    // Apply a meaning's mode-stack side-effect after the corresponding
    // token has been emitted. Every mutation to the frame stack lives
    // here (and at the endsAt-pop site inside the body branch) — no
    // ad-hoc push/pop scattered around the function.
    auto applyMeaningSideEffects = [&](LexemeMeaning const& m) {
        switch (m.modeOp) {
            case ModeOp::None: break;
            case ModeOp::PushMode: {
                StringStyle const* style = schema_->stringStyle(m);
                // Sanity check: pushing into a defaultToken-bearing
                // mode without a stringStyle would silently consume
                // the rest of the source (no escape, no close
                // detection). That's a schema-author bug; fail loud.
                if (style == nullptr
                    && m.modeArg.valid()
                    && schema_->lexerMode(m.modeArg).defaultToken.has_value()) {
                    tokenizerFatal("PushMode into a defaultToken-bearing mode without a stringStyle");
                }
                std::string captured;
                if (style && !style->tagPattern.empty()) {
                    // Dynamic tagPattern: capture at opener time so
                    // the body-mode close pattern is `endsAt + tag`.
                    // Cache keyed by `m.stringStyleId.v` — same
                    // StringStyle slot reused across multiple openers
                    // gets a single compiled regex.
                    auto it = tagRegexCache.find(m.stringStyleId.v);
                    if (it == tagRegexCache.end()) {
                        try {
                            it = tagRegexCache.emplace(m.stringStyleId.v,
                                                       std::regex(style->tagPattern)).first;
                        } catch (std::exception const&) {
                            // Loader pre-validates tagPattern (see
                            // grammar_schema_json), so a runtime
                            // construction failure means a deep config
                            // bug. Surface it loudly rather than
                            // silently swallowing — would otherwise
                            // mis-tokenize as static-endsAt only.
                            tokenizerFatal("tagPattern regex failed to compile at runtime");
                        }
                    }
                    std::cmatch match;
                    const auto remaining = r.remaining();
                    if (std::regex_search(remaining.data(),
                                          remaining.data() + remaining.size(),
                                          match, it->second,
                                          std::regex_constants::match_continuous)) {
                        captured.assign(match[0].first, match[0].second);
                        r.advance(static_cast<std::size_t>(match[0].length()));
                    }
                }
                frames.push_back(Frame{
                    .mode          = m.modeArg,
                    .style         = style,
                    .dynamicSuffix = std::move(captured),
                });
                break;
            }
            case ModeOp::PopMode: {
                // Main mode (depth 1) must never pop.
                if (frames.size() <= 1) {
                    tokenizerFatal("PopMode would underflow the frame stack (main mode unreachable)");
                }
                frames.pop_back();
                break;
            }
            case ModeOp::ReplaceMode: {
                // Replace must operate on a real body frame; replacing
                // main mode would break the depth-1 invariant.
                if (frames.size() <= 1) {
                    tokenizerFatal("ReplaceMode applied at main-mode depth");
                }
                StringStyle const* style = schema_->stringStyle(m);
                // The replaced frame starts with no dynamicSuffix.
                // Dynamic-tag capture happens only at pushMode time —
                // a replaceMode meaning that names a tagPattern-bearing
                // style would silently get an empty suffix here. No
                // shipped config exercises that combo; if a future one
                // does, capture logic needs to be hoisted out of the
                // PushMode arm and applied here too.
                frames.back() = Frame{
                    .mode          = m.modeArg,
                    .style         = style,
                    .dynamicSuffix = {},
                };
                break;
            }
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

        // Body-mode branch — when the current mode declares a
        // `defaultToken`, we're inside a string body, a comment, or
        // any other "consume until endsAt" construct. Resolution
        // order:
        //   1. doubled-delimiter escape (runs FIRST so `''` reads as
        //      literal `'`, not opener+closer)
        //   2. static- or dynamic-endsAt match (closes the body)
        //   3. char-escape (escapeKind::Char + escape lead char)
        //   4. one-codepoint fallback emitting the mode's defaultToken
        //
        // `flagsApplied` carries the mode's `defaultToken.flags` to
        // every body emission — that's how `EmptySpace` reaches the
        // AST for comment bodies (closes v2-gap-catalog row 3).
        const auto& topFrame = frames.back();
        const auto& currentMode = schema_->lexerMode(topFrame.mode);
        if (currentMode.defaultToken.has_value()) {
            const auto& bodyToken = *currentMode.defaultToken;
            StringStyle const* style = topFrame.style;
            std::string_view suffix{topFrame.dynamicSuffix};

            // Coalesced body: emit ONE in-grammar token spanning the whole body
            // (the same single-token model `IntLiteral` uses), then consume the
            // close delimiter without a token and pop the frame. Reuses the same
            // doubled-delimiter / endsAt / char-escape resolution as the
            // per-codepoint path below — only the emission granularity differs.
            // `start` is the first body byte (the opener was emitted last
            // iteration); the token spans [start, close-start) = the raw body,
            // which the lowering decodes (escapes resolved there, like ints).
            if (bodyToken.coalesce && style) {
                if (style->escapeKind == EscapeKind::Char && style->escapeChar == 0) {
                    tokenizerFatal("StringStyle escapeKind=Char with escapeChar=0 — schema bug");
                }
                bool sawClose = false;
                std::size_t closeLen = 0;
                while (!r.isAtEnd()) {
                    // doubled-delimiter escape: `''` is a literal delimiter, part of the body
                    if (style->escapeKind == EscapeKind::DoubledDelimiter
                        && r.remaining().size() >= 2 * style->endsAt.size()
                        && r.remaining().substr(0, style->endsAt.size()) == style->endsAt
                        && r.remaining().substr(style->endsAt.size(), style->endsAt.size()) == style->endsAt) {
                        r.advance(2 * style->endsAt.size());
                        continue;
                    }
                    // close delimiter?
                    if (const auto n = matchEndsAt(r, style->endsAt, suffix,
                                                   style->endsAtLongestMatch);
                        n > 0) {
                        sawClose = true;
                        closeLen = n;
                        break;
                    }
                    // char escape: lead byte + next codepoint are part of the body
                    if (style->escapeKind == EscapeKind::Char
                        && static_cast<unsigned char>(r.peek())
                               == static_cast<unsigned char>(style->escapeChar)) {
                        r.advance(1);
                        if (r.isAtEnd()) {
                            ParseDiagnostic d;
                            d.code     = DiagnosticCode::P_InvalidEscape;
                            d.severity = DiagnosticSeverity::Error;
                            d.buffer   = source_->id();
                            d.span     = SourceSpan::of(start, static_cast<ByteOffset>(r.position()));
                            d.actual   = "escape lead at end of source";
                            reporter_->report(std::move(d));
                            break;
                        }
                        r.advance(codepointByteCount(r.peek()));
                        continue;
                    }
                    // ordinary body codepoint
                    r.advance(codepointByteCount(r.peek()));
                }
                // One token for the whole body (possibly empty, e.g. `""`).
                emit(CoreTokenKind::Operator, bodyToken.kind, bodyToken.flags);
                if (sawClose) {
                    r.advance(closeLen);           // consume close delimiter (no token)
                    if (frames.size() <= 1) {
                        tokenizerFatal("frame stack underflow at coalesced endsAt");
                    }
                    frames.pop_back();
                }
                // else: EOF before close — frame stays open; the post-loop
                // unterminated-body logic emits P_UnterminatedString/Comment.
                continue;
            }

            if (style) {
                // 1. Doubled-delimiter escape: `''` inside a SQL
                //    string is a literal `'`, NOT the end of the body.
                if (style->escapeKind == EscapeKind::DoubledDelimiter
                    && r.remaining().size() >= 2 * style->endsAt.size()
                    && r.remaining().substr(0, style->endsAt.size()) == style->endsAt
                    && r.remaining().substr(style->endsAt.size(), style->endsAt.size()) == style->endsAt) {
                    r.advance(2 * style->endsAt.size());
                    emit(CoreTokenKind::Operator, bodyToken.kind, bodyToken.flags);
                    continue;
                }

                // 2. Static-or-dynamic endsAt match: consume + pop frame.
                if (const auto n = matchEndsAt(r, style->endsAt, suffix,
                                               style->endsAtLongestMatch);
                    n > 0) {
                    r.advance(n);
                    emit(CoreTokenKind::Operator, bodyToken.kind, bodyToken.flags);
                    if (frames.size() <= 1) {
                        tokenizerFatal("frame stack underflow at endsAt (main mode is unreachable)");
                    }
                    frames.pop_back();
                    continue;
                }

                // 3. Char-escape: lead byte + next codepoint emitted
                //    as one defaultToken. Permissive — any byte after
                //    the lead is accepted (P_InvalidEscape only fires
                //    when the lead is the last byte of the source).
                //
                //    A loader bug that sets escapeKind::Char without a
                //    real escapeChar would default to byte 0 and match
                //    every NUL byte in the source as an escape lead.
                //    Loader pre-validates per PR6, but a runtime sanity
                //    check is cheap belt-and-braces.
                if (style->escapeKind == EscapeKind::Char && style->escapeChar == 0) {
                    tokenizerFatal("StringStyle escapeKind=Char with escapeChar=0 — schema bug");
                }
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
                    emit(CoreTokenKind::Operator, bodyToken.kind, bodyToken.flags);
                    continue;
                }
            }

            // 4. Default: consume one codepoint as the body's default
            //    token kind. UTF-8 multi-byte sequences land in one
            //    token (per user-chosen per-codepoint granularity).
            r.advance(codepointByteCount(r.peek()));
            emit(CoreTokenKind::Operator, bodyToken.kind, bodyToken.flags);
            continue;
        }

        const char c = r.peek();

        // Active lexer mode for this main-scan position. The body-mode
        // branch above already `continue`d, so `frames.back()` is the
        // mode whose token table the operator/identifier lookups must
        // consult first (with global fallback). `frames` always holds at
        // least the always-on "main" frame, so `back()` is safe.
        const LexerModeId activeMode = frames.back().mode;

        // Whitespace runs emit one token per byte (matches the per-char
        // schema convention) — the pre-resolved kinds avoid the
        // longest-match loop for these known single-byte lookups.
        if (c == '\n') {
            r.advance(1);
            emit(CoreTokenKind::Newline, newlineKind);
            // Line-scoped mode auto-pop: a mode declared `popAtNewline`
            // (a C-directive / assembly-line mode) drops its frame here,
            // AFTER the newline token, so its lexing rules never leak
            // into the next line. The body-mode branch above already
            // `continue`d, so `frames.back()` is the active main-scan
            // mode. Loop, not a single pop, so a (degenerate) stack of
            // nested line-scoped modes all unwind at one newline. The
            // depth>1 guard keeps the always-on main frame.
            while (frames.size() > 1
                   && schema_->lexerMode(frames.back().mode).popAtNewline) {
                frames.pop_back();
            }
            continue;
        }
        if (isAsciiSpace(c)) {
            r.advance(1);
            emit(CoreTokenKind::Whitespace, wsKind);
            continue;
        }

        // Identifier or keyword: alphanumeric/underscore run. Multi-
        // char lexemes that START with an id-start byte but extend
        // past the id-run (`N'`, `B'`, `r"`, `b"`) need the global
        // longestMatch to win — probe id-run length non-destructively
        // first so we keep the original reader position when the
        // global lookup loses (`if`, `if_foo`, `Nxyz`).
        if (isIdStart(c)) {
            std::size_t identLen = 1;
            while (isIdContinue(r.peek(identLen))) ++identLen;

            const auto globalHit = longestMatchInMode(*schema_, activeMode,
                                                      r.remaining(), lexemeProbeMax);
            if (globalHit.length > identLen) {
                r.advance(globalHit.length);
                // coreKindForByte returns Operator for an id-start byte;
                // intentional — these matches are delimited-string openers
                // (`N'`) etc., never Word.
                emit(coreKindForByte(c), globalHit.meaning.id,
                     globalHit.meaning.flagsApplied);
                applyMeaningSideEffects(globalHit.meaning);
                continue;
            }

            r.advance(identLen);
            const auto lexeme = r.slice(start, r.position());
            const auto hit = longestMatchInMode(*schema_, activeMode,
                                                lexeme, lexemeProbeMax);
            // Only honor the lookup when it covers the entire run; a
            // schema entry like `int` shouldn't claim part of `integer`.
            const bool fullMatch = (hit.length == lexeme.size());
            const auto sk = fullMatch ? hit.meaning.id : InvalidSchemaToken;
            const auto wordFlags = fullMatch ? hit.meaning.flagsApplied
                                             : NodeFlags::None;
            emit(CoreTokenKind::Word, sk, wordFlags);
            if (fullMatch) applyMeaningSideEffects(hit.meaning);
            continue;
        }

        // Numeric literals — config-driven via `numberStyle` (08.55).
        // When the language declares no numeric grammar (no
        // `numberStyle` block) we fall through to the longest-match
        // path so a bare digit can still be lexed as part of a
        // language-specific token (or rejected as illegal).
        // Malformed prefix literals (`0x_` with no actual digit) still
        // get a token emitted but also produce P_MalformedNumber.
        //
        // Entry guard: the current byte is a decimal digit (when the
        // schema enables `decimal`) OR matches the first byte of any
        // declared integer prefix (so Pascal-style `$ff`, Erlang-style
        // `16#ff`, etc. enter the numeric scanner instead of the
        // longest-match operator path).
        auto startsNumberLiteral = [&]() -> bool {
            if (numberStyle == nullptr) return false;
            if (numberStyle->decimal && isDigit(c)) return true;
            for (auto const& p : numberStyle->integerPrefixes) {
                if (!p.prefix.empty() && p.prefix[0] == c) return true;
            }
            return false;
        };
        if (startsNumberLiteral()) {
            const auto scan = scanNumber(r, *numberStyle);
            const auto emitKind =
                scan.isFloat ? numberStyle->emitKind.floating
                             : numberStyle->emitKind.integer;
            emit(scan.isFloat ? CoreTokenKind::FloatLiteral
                              : CoreTokenKind::IntLiteral,
                 emitKind,
                 schema_->flagsForKind(emitKind));
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

        // Operator or punctuation: longest-match against the active
        // mode's override table first, then the schema's global lexeme
        // keys, bounded by the schema's longest key length (no silent
        // truncation on a future 5+ char lexeme).
        const auto hit = longestMatchInMode(*schema_, activeMode,
                                            r.remaining(), lexemeProbeMax);
        if (hit.length > 0) {
            r.advance(hit.length);
            emit(coreKindForByte(c), hit.meaning.id, hit.meaning.flagsApplied);
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

    // Unterminated body modes: source ended before the active body's
    // endsAt was matched. Emit one diagnostic per unterminated frame
    // (excluding the always-on "main" at frames[0]). The diagnostic
    // flavor comes from the schema-declared `unterminatedFlavor` on
    // the mode — no more substring-sniffing the mode name. Generic
    // falls back to P_UnterminatedString since that's the closest
    // single-code match.
    while (frames.size() > 1) {
        const auto& mode = schema_->lexerMode(frames.back().mode);
        // A line-scoped (`popAtNewline`) mode that reaches EOF without a
        // trailing newline is NOT unterminated — EOF is a valid end of
        // the last line (a file ending in `#include "x.h"` with no final
        // newline is well-formed C). Close it silently, like the newline
        // pop would have.
        if (mode.popAtNewline) {
            frames.pop_back();
            continue;
        }
        ParseDiagnostic d;
        switch (mode.unterminatedFlavor) {
            case UnterminatedFlavor::Comment:
                d.code = DiagnosticCode::P_UnterminatedComment;
                break;
            case UnterminatedFlavor::String:
            case UnterminatedFlavor::Generic:
                d.code = DiagnosticCode::P_UnterminatedString;
                break;
        }
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = source_->id();
        d.span     = SourceSpan::of(static_cast<ByteOffset>(r.size()),
                                    static_cast<ByteOffset>(r.size()));
        d.actual   = std::format("EOF inside lexer mode '{}'", mode.name);
        reporter_->report(std::move(d));
        frames.pop_back();
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
