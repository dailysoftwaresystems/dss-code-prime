#pragma once

// C11/C23 6.4.5 wide/UTF string-literal ENCODING — the shared code-unit engine
// for a NON-NARROW string opener (`L"…"`/`u"…"`/`U"…"`/`u8"…"`). The tokenizer
// captures RAW source bytes (UTF-8 on disk), so `u"€"`'s body is the three bytes
// `E2 82 AC`; this header decodes those to code points and re-encodes each into
// the target element width, so both the SEMANTIC tier (which needs the code-unit
// COUNT for the `Array<core, N+1>` type) and the CST→HIR tier (which needs the
// encoded BYTES) derive from ONE implementation and can never disagree on N.
//
// STRICT + FAIL-LOUD by design (never a silent truncation): ill-formed UTF-8, a
// code point past U+10FFFF, and a supplementary-plane code point under a 16-bit
// element (surrogate pairs are a LATER cycle) each stop with a distinct error the
// caller renders as a diagnostic. The NARROW (`Char`) path does NOT route here —
// it keeps its existing byte-level `\`-escape decode (raw ≥0x80 passthrough).

#include "core/types/type_lattice/core_type.hpp"   // TypeKind

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

// NOTE: the wchar_t (`L"…"`) element width is FORMAT-keyed, but that resolution is
// CONFIG-DRIVEN — declared per language via `LiteralPrefixEntry::elementCoreByFormat`
// (mirroring builtinTypes' `coreByDataModel`) and resolved by a pure config-map
// lookup (`resolveElementCore`). This header holds only the format-INVARIANT encode
// machinery; it intentionally does NOT branch on `ObjectFormatKind`.

// Why a wide-string encode failed. Each maps to a caller diagnostic; the code
// unit count / bytes are NOT produced on failure (no guessed size).
enum class WideEncodeError : std::uint8_t {
    IllFormedUtf8,      // the raw body is not valid UTF-8 (truncated / stray continuation)
    CodepointTooLarge,  // a decoded code point exceeds U+10FFFF (not a Unicode scalar value)
    SurrogateUnsupported, // a supplementary-plane cp (> U+FFFF) under a 16-bit element core
};

// Decode `bytes` (raw UTF-8, escapes ALREADY resolved by the byte decoder) into
// code points. Strict: rejects over-long forms, stray continuation bytes, a
// truncated trailing sequence, and any value past U+10FFFF. Returns the error on
// the FIRST malformed unit (caller fails loud); `out` is cleared on entry and is
// left partial on failure (callers discard it).
[[nodiscard]] inline std::optional<WideEncodeError>
decodeUtf8ToCodepoints(std::string_view bytes, std::vector<char32_t>& out) {
    out.clear();
    std::size_t i = 0;
    auto const n = bytes.size();
    auto cont = [&](std::size_t at) -> bool {
        return at < n && (static_cast<unsigned char>(bytes[at]) & 0xC0) == 0x80;
    };
    while (i < n) {
        unsigned char const b0 = static_cast<unsigned char>(bytes[i]);
        std::uint32_t cp = 0;
        std::size_t len = 0;
        std::uint32_t lo = 0;   // lowest value legal for this length (over-long guard)
        if (b0 < 0x80) { cp = b0; len = 1; lo = 0; }
        else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1Fu; len = 2; lo = 0x80; }
        else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0Fu; len = 3; lo = 0x800; }
        else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07u; len = 4; lo = 0x10000; }
        else return WideEncodeError::IllFormedUtf8;   // 0x80..0xBF lead / 0xF8+
        for (std::size_t k = 1; k < len; ++k) {
            if (!cont(i + k)) return WideEncodeError::IllFormedUtf8;
            cp = (cp << 6) | (static_cast<unsigned char>(bytes[i + k]) & 0x3Fu);
        }
        if (cp < lo) return WideEncodeError::IllFormedUtf8;        // over-long encoding
        if (cp > 0x10FFFF) return WideEncodeError::CodepointTooLarge;
        if (cp >= 0xD800 && cp <= 0xDFFF)                          // UTF-16 surrogate half
            return WideEncodeError::IllFormedUtf8;                 // not a scalar value
        out.push_back(static_cast<char32_t>(cp));
        i += len;
    }
    return std::nullopt;
}

// Append the code units for `cp` in element width `core` to `out`, growing it by
// the element's byte width per unit. LE for the multi-byte element cores (DSS
// targets are little-endian). Returns SurrogateUnsupported when `cp` needs a
// surrogate pair under a 16-bit core (a LATER cycle), else std::nullopt.
//   U8            → UTF-8 bytes (1..4; BMP is 1..3)
//   U16           → one 2-byte LE unit for BMP (cp>0xFFFF fails loud)
//   U32 / I32     → one 4-byte LE unit (I32 = wchar_t on elf/macho)
//   Char (narrow) → one truncated low byte (only reached if a caller routes a
//                   narrow core here; the normal narrow path decodes bytes directly)
[[nodiscard]] inline std::optional<WideEncodeError>
encodeCodepoint(char32_t cp, TypeKind core, std::string& out) {
    auto put = [&](std::uint32_t v) { out.push_back(static_cast<char>(static_cast<unsigned char>(v & 0xFFu))); };
    switch (core) {
        case TypeKind::U8: {
            std::uint32_t const c = cp;
            if (c < 0x80) { put(c); }
            else if (c < 0x800) { put(0xC0 | (c >> 6)); put(0x80 | (c & 0x3F)); }
            else if (c < 0x10000) {
                put(0xE0 | (c >> 12)); put(0x80 | ((c >> 6) & 0x3F)); put(0x80 | (c & 0x3F));
            } else {
                put(0xF0 | (c >> 18)); put(0x80 | ((c >> 12) & 0x3F));
                put(0x80 | ((c >> 6) & 0x3F)); put(0x80 | (c & 0x3F));
            }
            return std::nullopt;
        }
        case TypeKind::U16: {
            if (cp > 0xFFFF) return WideEncodeError::SurrogateUnsupported;
            put(cp & 0xFF); put((cp >> 8) & 0xFF);
            return std::nullopt;
        }
        case TypeKind::U32:
        case TypeKind::I32: {
            put(cp & 0xFF); put((cp >> 8) & 0xFF); put((cp >> 16) & 0xFF); put((cp >> 24) & 0xFF);
            return std::nullopt;
        }
        default: {
            // A narrow core reaching here (defensive): one low byte, matching the
            // pre-wide behavior for a truncated char.
            put(cp & 0xFF);
            return std::nullopt;
        }
    }
}

// One code unit's byte width for element `core` (U16→2, U32/I32→4, U8/Char→1).
// The stride the semantic array-count → byte-size conversion uses when a caller
// has the code-unit count and needs the flat byte length.
[[nodiscard]] inline std::uint32_t elementByteWidth(TypeKind core) noexcept {
    switch (core) {
        case TypeKind::U16:              return 2;
        case TypeKind::U32:
        case TypeKind::I32:              return 4;
        default:                         return 1;   // U8 / Char
    }
}

// Result of encoding a whole (escape-decoded) body under a non-narrow core.
struct WideEncodeResult {
    std::string   bytes;        // the encoded code units (LE), no trailing NUL
    std::uint64_t codeUnits;    // number of ELEMENT-width units (Array length = this + 1)
};

// Decode `escaped` (bytes after `\`-escape resolution) as UTF-8 and re-encode
// into `core`, returning the encoded bytes + code-unit count, or the first error.
// This is THE shared path: the semantic typer takes `.codeUnits` for the array
// length, the HIR lowerer takes `.bytes` for the literal — one computation.
[[nodiscard]] inline std::optional<WideEncodeError>
encodeWideString(std::string_view escaped, TypeKind core, WideEncodeResult& out) {
    std::vector<char32_t> cps;
    if (auto err = decodeUtf8ToCodepoints(escaped, cps)) return err;
    out.bytes.clear();
    std::uint32_t const width = elementByteWidth(core);
    std::size_t const before = out.bytes.size();
    for (char32_t cp : cps) {
        if (auto err = encodeCodepoint(cp, core, out.bytes)) return err;
    }
    // Byte length is always a whole multiple of the element width (each unit is
    // `width` bytes); the code-unit count is that quotient. For U8 the "unit" is
    // one byte so this is exactly the encoded length.
    std::size_t const encoded = out.bytes.size() - before;
    out.codeUnits = static_cast<std::uint64_t>(encoded / width);
    return std::nullopt;
}

} // namespace dss
