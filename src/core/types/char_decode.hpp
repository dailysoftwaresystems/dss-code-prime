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

// The result of decoding a char/string-literal body. `error` names WHY the
// decode failed (None on success); `usedByteEscape` records whether ANY `\x` or
// octal `\ooo` byte-escape was consumed (true even on an otherwise-successful
// decode). A byte escape names a raw code-unit VALUE, not a code POINT — the
// wide/UTF literal paths reject it fail-loud (the escape-value-as-code-unit
// feature is deferred: D-CSUBSET-WIDE-HEX-OCTAL-ESCAPE-VALUE), while the narrow
// byte path keeps it. `error` separates a plain malformed escape from an invalid
// universal character name so callers can render the specific diagnostic.
enum class EscapeDecodeError : std::uint8_t {
    None,                  // success
    Malformed,             // unknown escape, bad \x/octal, or a trailing lone backslash
    InvalidUniversalName,  // \u/\U: fewer than 4/8 hex digits, a surrogate half, or > U+10FFFF
};

struct EscapeDecodeOutcome {
    EscapeDecodeError error          = EscapeDecodeError::None;
    bool              usedByteEscape = false;
    [[nodiscard]] bool ok() const noexcept { return error == EscapeDecodeError::None; }
};

// Decode C-family `\`-escapes in `body`, appending raw bytes to `out`. Returns
// an EscapeDecodeOutcome whose `.ok()` is false on a malformed/unknown/out-of-
// range escape or an invalid `\u`/`\U` (caller fails loud); the partial output is
// undefined on failure. Supported: \n \t \r \\ \' \" \a \b \f \v, octal \ooo
// (one-to-three digits, \0..\377), \xHH (one-or-two hex digits), and the C11/C23
// 6.4.3 universal character names \uXXXX (EXACTLY 4 hex) / \UXXXXXXXX (EXACTLY 8
// hex). A backslash before any other byte is rejected rather than silently passed
// through — no guessing.
//
// A UCN is appended as the CANONICAL single-code-point UTF-8 encoding — uniform
// for narrow (→ UTF-8 execution-charset bytes) and wide (→ re-decoded by the
// wide encoder). FF1: a `\u`/`\U` naming a UTF-16 surrogate half (U+D800..U+DFFF)
// or a value past U+10FFFF is rejected HERE, before the append — the narrow path
// has no downstream UTF-8 validation, so an unchecked UCN would emit CESU-8 /
// overlong bytes silently. `.usedByteEscape` flags a consumed `\x`/octal escape
// (the wide path fails loud on it); UCNs name code POINTS and are valid UTF-8 by
// construction, so they never set the flag.
[[nodiscard]] inline EscapeDecodeOutcome decodeEscapedBytes(std::string_view body, std::string& out) {
    auto const hexVal = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return 10 + (h - 'a');
        if (h >= 'A' && h <= 'F') return 10 + (h - 'A');
        return -1;
    };
    auto const appendUtf8 = [&out](std::uint32_t cp) {
        auto push = [&out](std::uint32_t v) {
            out.push_back(static_cast<char>(static_cast<unsigned char>(v & 0xFFu)));
        };
        if (cp < 0x80) {
            push(cp);
        } else if (cp < 0x800) {
            push(0xC0 | (cp >> 6)); push(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            push(0xE0 | (cp >> 12)); push(0x80 | ((cp >> 6) & 0x3F)); push(0x80 | (cp & 0x3F));
        } else {
            push(0xF0 | (cp >> 18)); push(0x80 | ((cp >> 12) & 0x3F));
            push(0x80 | ((cp >> 6) & 0x3F)); push(0x80 | (cp & 0x3F));
        }
    };

    bool usedByteEscape = false;
    for (std::size_t i = 0; i < body.size(); ++i) {
        char const c = body[i];
        if (c != '\\') { out.push_back(c); continue; }
        if (i + 1 >= body.size())
            return {EscapeDecodeError::Malformed, usedByteEscape};   // trailing lone backslash
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
            if (v > 0xFF)
                return {EscapeDecodeError::Malformed, usedByteEscape};   // octal past unsigned-char range
            out.push_back(static_cast<char>(static_cast<unsigned char>(v)));
            usedByteEscape = true;
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
                if (i + 1 >= body.size()) return {EscapeDecodeError::Malformed, usedByteEscape};
                int v = hexVal(body[i + 1]);
                if (v < 0) return {EscapeDecodeError::Malformed, usedByteEscape};
                ++i;
                if (i + 1 < body.size()) {
                    int const v2 = hexVal(body[i + 1]);
                    if (v2 >= 0) { v = v * 16 + v2; ++i; }
                }
                out.push_back(static_cast<char>(static_cast<unsigned char>(v)));
                usedByteEscape = true;
                break;
            }
            case 'u':
            case 'U': {
                // C11/C23 6.4.3 universal character name: \u = EXACTLY 4 hex
                // digits, \U = EXACTLY 8. Fewer, or a non-hex digit, is malformed.
                int const want = (e == 'u') ? 4 : 8;
                std::uint32_t cp = 0;
                for (int d = 0; d < want; ++d) {
                    if (i + 1 >= body.size())
                        return {EscapeDecodeError::InvalidUniversalName, usedByteEscape};
                    int const hv = hexVal(body[i + 1]);
                    if (hv < 0)
                        return {EscapeDecodeError::InvalidUniversalName, usedByteEscape};
                    cp = cp * 16 + static_cast<std::uint32_t>(hv);
                    ++i;
                }
                // FF1: reject a UTF-16 surrogate half / a value past U+10FFFF
                // BEFORE the append (the narrow path has no downstream UTF-8
                // validation). C23 6.4.3 relaxed the <0x00A0 basic-character
                // restriction for string/character LITERALS, so that constraint is
                // intentionally NOT enforced here (`A` = "A" is valid).
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                    return {EscapeDecodeError::InvalidUniversalName, usedByteEscape};
                appendUtf8(cp);
                break;
            }
            default:
                return {EscapeDecodeError::Malformed, usedByteEscape};   // unknown escape — fail loud, never guess
        }
    }
    return {EscapeDecodeError::None, usedByteEscape};
}

// Decode a string-literal body to its byte sequence (escapes resolved). The
// result is NOT NUL-terminated — the trailing NUL is implied by the literal's
// Array<Char, N+1> type. std::nullopt on a malformed escape. `outcome` (when
// non-null) receives the specific failure reason + the byte-escape flag so a
// caller can render the exact diagnostic and enforce the wide-literal ban on
// `\x`/octal escapes.
[[nodiscard]] inline std::optional<std::string>
decodeStringLiteralBody(std::string_view body, EscapeDecodeOutcome* outcome = nullptr) {
    std::string out;
    out.reserve(body.size());
    EscapeDecodeOutcome const oc = decodeEscapedBytes(body, out);
    if (outcome) *outcome = oc;
    if (!oc.ok()) return std::nullopt;
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
// malformed-escape bodies return std::nullopt (caller fails loud). `outcome`
// (when non-null) receives the specific failure reason + the byte-escape flag;
// note `.ok()` can be true while the result is still nullopt (a valid multi-byte
// UCN such as `é` decodes to >1 byte — not a single narrow char).
[[nodiscard]] inline std::optional<std::uint32_t>
decodeCharLiteralBody(std::string_view body, EscapeDecodeOutcome* outcome = nullptr) {
    std::string out;
    EscapeDecodeOutcome const oc = decodeEscapedBytes(body, out);
    if (outcome) *outcome = oc;
    if (!oc.ok()) return std::nullopt;
    if (out.size() != 1) return std::nullopt;   // empty or multi-char
    return static_cast<std::uint32_t>(static_cast<unsigned char>(out[0]));
}

} // namespace dss
