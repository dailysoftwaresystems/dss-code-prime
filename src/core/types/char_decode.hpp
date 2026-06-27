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
// false on a malformed/unknown/out-of-range escape (caller fails loud); the
// partial output is undefined on failure. Supported: \n \t \r \\ \' \" \a \b \f
// \v, octal \ooo (one-to-three digits, \0..\377) and \xHH (one-or-two hex
// digits). A backslash before any other byte is rejected rather than silently
// passed through — no guessing.
[[nodiscard]] inline bool decodeEscapedBytes(std::string_view body, std::string& out) {
    for (std::size_t i = 0; i < body.size(); ++i) {
        char const c = body[i];
        if (c != '\\') { out.push_back(c); continue; }
        if (i + 1 >= body.size()) return false;   // trailing lone backslash
        char const e = body[++i];
        // Octal escape `\ooo` (C 6.4.4.4): one-to-THREE octal digits in the
        // unsigned-char range (\0..\377). Handled BEFORE the named-escape switch
        // so `\0`, `\07`, `\101`, `\301` all decode as octal (the old `case '0'`
        // only covered a bare `\0` and would mis-split `\012` into `\0` + "12").
        // An out-of-range value (\400..\777, i.e. > 255) is a constraint
        // violation (C 6.4.4.4p9) and fails loud — NEVER silently masked. A lone
        // `\8`/`\9` is NOT octal — it falls through to the switch's `default` and
        // fails loud.
        if (e >= '0' && e <= '7') {
            int v = e - '0';
            for (int d = 0; d < 2 && i + 1 < body.size()
                            && body[i + 1] >= '0' && body[i + 1] <= '7'; ++d) {
                v = v * 8 + (body[++i] - '0');
            }
            if (v > 0xFF) return false;   // octal escape past unsigned-char range — fail loud
            out.push_back(static_cast<char>(static_cast<unsigned char>(v)));
            continue;
        }
        switch (e) {
            case 'n':  out.push_back('\n'); break;
            case 't':  out.push_back('\t'); break;
            case 'r':  out.push_back('\r'); break;
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

// Decode a DOUBLED-DELIMITER string body (SQL `'…''…'`): a doubled `delimiter`
// is one literal delimiter byte; every other byte passes through. The coalesced
// body the tokenizer captured contains `''` pairs for embedded quotes and no
// lone delimiter (the closer was consumed on mode-pop), so this never fails.
[[nodiscard]] inline std::string
decodeDoubledDelimiterBody(std::string_view body, char delimiter) {
    std::string out;
    out.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == delimiter && i + 1 < body.size() && body[i + 1] == delimiter) {
            out.push_back(delimiter);
            ++i;   // consume the second delimiter of the pair
            continue;
        }
        out.push_back(body[i]);
    }
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
