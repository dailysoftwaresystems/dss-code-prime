#pragma once

// Shared decoder for the BODY of a char/string literal — turns the raw source
// bytes between the delimiters (escapes unresolved, as the tokenizer's coalesced
// body token captures them) into the decoded value. One implementation feeds
// both char-literal lowering (→ a single codepoint) and string-literal lowering
// (→ the decoded byte sequence), so an escape is interpreted identically in
// both. C-family `\`-escapes are handled here; a doubled-delimiter scheme
// (SQL `''`) is a separate additive decoder selected by the literal's style.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dss {

// Decode C-family `\`-escapes in `body`, appending raw bytes to `out`. Returns
// false on a malformed/unknown escape (caller fails loud); the partial output is
// undefined on failure. Supported: \n \t \r \0 \\ \' \" \a \b \f \v and \xHH
// (one-or-two hex digits). A backslash before any other byte is rejected rather
// than silently passed through — no guessing.
[[nodiscard]] inline bool decodeEscapedBytes(std::string_view body, std::string& out) {
    for (std::size_t i = 0; i < body.size(); ++i) {
        char const c = body[i];
        if (c != '\\') { out.push_back(c); continue; }
        if (i + 1 >= body.size()) return false;   // trailing lone backslash
        char const e = body[++i];
        switch (e) {
            case 'n':  out.push_back('\n'); break;
            case 't':  out.push_back('\t'); break;
            case 'r':  out.push_back('\r'); break;
            case '0':  out.push_back('\0'); break;
            case '\\': out.push_back('\\'); break;
            case '\'': out.push_back('\''); break;
            case '"':  out.push_back('"');  break;
            case 'a':  out.push_back('\a'); break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'v':  out.push_back('\v'); break;
            case 'x': {
                // \xH or \xHH — one or two hex digits.
                auto hexVal = [](char h) -> int {
                    if (h >= '0' && h <= '9') return h - '0';
                    if (h >= 'a' && h <= 'f') return 10 + (h - 'a');
                    if (h >= 'A' && h <= 'F') return 10 + (h - 'A');
                    return -1;
                };
                if (i + 1 >= body.size()) return false;
                int v = hexVal(body[i + 1]);
                if (v < 0) return false;
                ++i;
                if (i + 1 < body.size()) {
                    int const v2 = hexVal(body[i + 1]);
                    if (v2 >= 0) { v = v * 16 + v2; ++i; }
                }
                out.push_back(static_cast<char>(static_cast<unsigned char>(v)));
                break;
            }
            default: return false;   // unknown escape — fail loud, never guess
        }
    }
    return true;
}

// Decode a string-literal body to its byte sequence (escapes resolved). The
// result is NOT NUL-terminated — the trailing NUL is implied by the literal's
// Array<Char, N+1> type. std::nullopt on a malformed escape.
[[nodiscard]] inline std::optional<std::string> decodeStringLiteralBody(std::string_view body) {
    std::string out;
    out.reserve(body.size());
    if (!decodeEscapedBytes(body, out)) return std::nullopt;
    return out;
}

// Decode a char-literal body to a single codepoint. The body must resolve to
// EXACTLY one byte (ASCII or a single-byte escape); empty, multi-byte, or
// malformed-escape bodies return std::nullopt (caller fails loud).
[[nodiscard]] inline std::optional<std::uint32_t> decodeCharLiteralBody(std::string_view body) {
    std::string out;
    if (!decodeEscapedBytes(body, out)) return std::nullopt;
    if (out.size() != 1) return std::nullopt;   // empty or multi-char
    return static_cast<std::uint32_t>(static_cast<unsigned char>(out[0]));
}

} // namespace dss
