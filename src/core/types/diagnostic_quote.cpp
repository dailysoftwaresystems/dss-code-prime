#include "core/types/diagnostic_quote.hpp"

// The diagnostics' quoted-source-text limit, forwarded from its configure-time document by
// src/core/CMakeLists.txt (cmake/DssConfigureTimeCount.cmake reads it). No default.
#if !defined(DSS_DIAGNOSTIC_QUOTE_LIMIT)
#error "the diagnostics' quoted-source-text limit was not forwarded: src/core/CMakeLists.txt reads it from src/dss-config/configure-time/diagnostics.json"
#endif

namespace dss {

namespace {

[[nodiscard]] constexpr bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

[[nodiscard]] constexpr bool isLineBreak(char c) noexcept { return c == '\n' || c == '\r'; }

[[nodiscard]] constexpr bool isContinuationByte(char c) noexcept {
    return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u;
}

// The bytes of the UTF-8 sequence a lead byte opens (1 for ASCII and for a stray byte).
[[nodiscard]] constexpr std::size_t sequenceLength(char lead) noexcept {
    auto const b = static_cast<unsigned char>(lead);
    if ((b & 0xE0u) == 0xC0u) return 2;
    if ((b & 0xF0u) == 0xE0u) return 3;
    if ((b & 0xF8u) == 0xF0u) return 4;
    return 1;
}

constexpr std::string_view kEllipsis = "…";

} // namespace

std::size_t diagnosticQuoteLimit() noexcept {
    return static_cast<std::size_t>(DSS_DIAGNOSTIC_QUOTE_LIMIT);
}

std::string quotedSourceText(std::string_view text, std::size_t limit) {
    std::string out;
    out.reserve(text.size() < limit ? text.size() : limit);
    bool cut = false;
    std::size_t i = 0;
    while (i < text.size() && isSpace(text[i])) ++i;   // leading white space goes
    while (i < text.size()) {
        if (isSpace(text[i])) {
            // A run of white space: the source's own bytes, unless it holds a line
            // break, which makes it one space. A run that ends the text goes.
            std::size_t j = i;
            bool lineBreak = false;
            while (j < text.size() && isSpace(text[j])) {
                lineBreak = lineBreak || isLineBreak(text[j]);
                ++j;
            }
            if (j == text.size()) break;
            std::string_view const run =
                lineBreak ? std::string_view{" "} : text.substr(i, j - i);
            if (out.size() + run.size() > limit) { cut = true; break; }
            out += run;
            i = j;
            continue;
        }
        // One character: its whole UTF-8 sequence, or nothing of it.
        // A malformed or truncated sequence is quoted byte by byte.
        std::size_t n = sequenceLength(text[i]);
        if (i + n > text.size()) n = 1;
        for (std::size_t k = 1; k < n; ++k) {
            if (!isContinuationByte(text[i + k])) { n = 1; break; }
        }
        if (out.size() + n > limit) { cut = true; break; }
        out.append(text.substr(i, n));
        i += n;
    }
    if (cut) {
        while (!out.empty() && isSpace(out.back())) out.pop_back();
        out += kEllipsis;
    }
    return out;
}

std::string quotedSourceText(std::string_view text) {
    return quotedSourceText(text, diagnosticQuoteLimit());
}

} // namespace dss
