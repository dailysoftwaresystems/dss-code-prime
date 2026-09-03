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
#include <vector>

namespace dss {

// ONE consumed `\x` hex / `\ooo` octal escape, recorded in output order.
//
// ★ THE WHOLE POINT: a byte escape names a raw CODE-UNIT VALUE, and that value
// can be WIDER than the one placeholder byte the decoder can push into a byte
// buffer (`u"\xFFFF"` is one 0xFFFF unit; `U"\xFFFFFFFF"` is one 0xFFFFFFFF
// unit). Flattening the escape into bytes and re-reading them is what produced
// the two defects this record closes — the wide SILENT COLLAPSE through the
// UTF-8 re-decoder (D-CSUBSET-WIDE-HEX-OCTAL-ESCAPE-VALUE) and the NARROW
// two-hex-digit truncation (D-CSUBSET-NARROW-HEX-ESCAPE-TRUNCATED-TO-TWO-DIGITS).
// So the decoder keeps the VALUE beside the buffer: `byteOffset` says where the
// escape's placeholder byte sits in the decoded buffer, and `value` is what the
// escape actually named. A consumer that knows the element width splices the
// value in as one code unit; a consumer that only wants bytes reads the buffer
// exactly as before.
//
// ⚠ `byteOffset` is an offset into the buffer THIS decode wrote. An adjacent-
// concatenated run decodes per segment (C 5.1.1.2 phase 5) and joins the bytes
// (phase 6), so a joiner MUST REBASE each segment's offset by the running join
// length — see `decodeAdjacentStringBodies`.
struct EscapeValueUnit {
    std::size_t   byteOffset = 0;      // offset in the decoded buffer of the placeholder byte
    std::uint64_t value      = 0;      // the RAW escape value — MAY exceed 0xFF
    bool          hex        = false;  // `\xHH…` (unbounded digits) vs `\ooo` (≤ 3 digits)
};

// The result of decoding a char/string-literal body. `error` names WHY the
// decode failed (None on success); `usedByteEscape` records whether ANY `\x` or
// octal `\ooo` byte-escape was consumed (true even on an otherwise-successful
// decode); `escapeUnits` carries each one's offset + raw value.
//
// ★★ NO RANGE CHECK LIVES HERE, and that is deliberate rather than an omission.
// The legal range of an escape is the range of the ELEMENT the literal is made
// of, and the decoder does not know the element — `\777` (0x1FF) is a constraint
// violation in `"…"` and a perfectly ordinary unit in `u"…"`, and all four
// reference compilers agree on both halves. The check is `firstEscapeValueTooWide`,
// which the consumer calls with the width it knows. (The previous decoder
// rejected octal > 0xFF here, which is why `u"\777"` was refused as malformed.)
enum class EscapeDecodeError : std::uint8_t {
    None,                  // success
    Malformed,             // unknown escape, a `\x` with no digit, or a trailing lone backslash
    InvalidUniversalName,  // \u/\U: fewer than 4/8 hex digits, a surrogate half, or > U+10FFFF
    EscapeValueTooLarge,   // a `\x` whose value exceeds 64 bits — wider than ANY code unit
};

struct EscapeDecodeOutcome {
    EscapeDecodeError            error          = EscapeDecodeError::None;
    bool                         usedByteEscape = false;
    std::vector<EscapeValueUnit> escapeUnits;
    [[nodiscard]] bool ok() const noexcept { return error == EscapeDecodeError::None; }
};

// The FIRST escape whose value does not fit a `unitBits`-wide code unit, or
// nullopt. THE single range check for both defects: the narrow byte path passes
// 8, a wide/UTF path passes `8 * elementByteWidth(core)`. Returning the UNIT
// (not a bool) is load-bearing — the diagnostic has to name the width AND the
// value, because "this escape is out of range" without either is a message that
// cannot be acted on.
//
// ⚠ The reference compilers SPLIT on the overflow case and the split decides
// this signature: gcc 13.3.0 and mingw-w64 gcc 13.2.0 accept by TRUNCATING with
// a warning (`u"\x1FFFF"` → 0xFFFF), clang 18.1.3 and MSVC 19.51 REFUSE. A
// reference that only accepts by silently narrowing the value is not a reference
// that WORKS for that literal, so the union taken over what works is a REFUSAL —
// never a truncation, which is exactly the silent wrong answer both anchors name.
[[nodiscard]] inline std::optional<EscapeValueUnit>
firstEscapeValueTooWide(EscapeDecodeOutcome const& outcome, unsigned unitBits) noexcept {
    std::uint64_t const limit =
        unitBits >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << unitBits) - 1u);
    for (EscapeValueUnit const& u : outcome.escapeUnits) {
        if (u.value > limit) return u;
    }
    return std::nullopt;
}

// Decode C-family `\`-escapes in `body`, appending raw bytes to `out`. Returns
// an EscapeDecodeOutcome whose `.ok()` is false on a malformed/unknown escape or
// an invalid `\u`/`\U` (caller fails loud); the partial output is undefined on
// failure. Supported: \n \t \r \\ \' \" \a \b \f \v, octal \ooo (one-to-THREE
// digits), \xH… (an UNBOUNDED run of hex digits), and the C11/C23 6.4.3 universal
// character names \uXXXX (EXACTLY 4 hex) / \UXXXXXXXX (EXACTLY 8 hex). A
// backslash before any other byte is rejected rather than silently passed
// through — no guessing.
//
// ★★ `\x` CONSUMES EVERY FOLLOWING HEX DIGIT (C 6.4.4.4p7 — "the hexadecimal
// digits that follow … are taken to be part of the construction of a single
// character"), and the old one-or-two-digit cap was a SILENT WRONG ANSWER in the
// NARROW path: ✔MEASURED 2026-09-02 through the shipped CLI, `"\x041"` emitted
// `04 31 00` where gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0 and MSVC 19.51
// ALL emit `41 00`, and `"a\xFFb"` emitted `61 FF 62 00` — matching NO reference,
// since `b` IS a hex digit and the escape is `\xFFb`
// (D-CSUBSET-NARROW-HEX-ESCAPE-TRUNCATED-TO-TWO-DIGITS). Octal is genuinely
// capped at three digits, so `"a\101b"` keeps its trailing `b` — the two escape
// kinds terminate differently and that asymmetry is the standard's, not ours.
//
// ★ NEITHER escape kind is RANGE-checked here — see `EscapeDecodeError`. Each is
// recorded in `.escapeUnits` with its raw value and its offset in `out`, and the
// low byte is pushed as a placeholder so a byte-only consumer reads exactly what
// it read before. The one range failure that IS width-independent — a `\x` whose
// value needs more than 64 bits, wider than any code unit that exists — stops
// here as EscapeValueTooLarge.
//
// A UCN is appended as the CANONICAL single-code-point UTF-8 encoding — uniform
// for narrow (→ UTF-8 execution-charset bytes) and wide (→ re-decoded by the
// wide encoder). FF1: a `\u`/`\U` naming a UTF-16 surrogate half (U+D800..U+DFFF)
// or a value past U+10FFFF is rejected HERE, before the append — the narrow path
// has no downstream UTF-8 validation, so an unchecked UCN would emit CESU-8 /
// overlong bytes silently. ⚠ That check is the UCN's ALONE and must not be
// generalised to byte escapes: ✔MEASURED, all four references assemble
// `u"\xD800"` into one 0xD800 unit while ALL FOUR refuse `u"\uD800"` — unanimous
// in BOTH directions, which is the proof that a byte escape is not a code point.
// `.usedByteEscape` flags a consumed `\x`/octal escape; UCNs name code POINTS and
// are valid UTF-8 by construction, so they never set it and never appear in
// `.escapeUnits`.
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

    EscapeDecodeOutcome oc;
    auto const fail = [&oc](EscapeDecodeError e) -> EscapeDecodeOutcome {
        oc.error = e;
        return std::move(oc);
    };
    // Record one consumed byte escape: its raw value, and the offset of the
    // placeholder low byte pushed for it.
    auto const recordEscape = [&oc, &out](std::uint64_t v, bool hex) {
        oc.escapeUnits.push_back({out.size(), v, hex});
        out.push_back(static_cast<char>(static_cast<unsigned char>(v & 0xFFu)));
        oc.usedByteEscape = true;
    };
    for (std::size_t i = 0; i < body.size(); ++i) {
        char const c = body[i];
        if (c != '\\') { out.push_back(c); continue; }
        if (i + 1 >= body.size())
            return fail(EscapeDecodeError::Malformed);   // trailing lone backslash
        char const e = body[++i];
        // Octal escape `\ooo` (C 6.4.4.4): one-to-THREE octal digits. Handled
        // BEFORE the named-escape switch so `\0`, `\07`, `\101`, `\301` all decode
        // as octal (the old `case '0'` only covered a bare `\0` and would mis-split
        // `\012` into `\0` + "12"). A lone `\8`/`\9` is NOT octal — it falls
        // through to the switch's `default` and fails loud.
        //
        // ⚠ THE `> 0xFF` REFUSAL THAT USED TO LIVE HERE HAS MOVED, not vanished.
        // Three digits cap the value at 0777 = 0x1FF, which overflows a narrow
        // char and fits a `char16_t` — ✔MEASURED, all four references emit one
        // 0x1FF unit for `u"\777"`, while narrow `"\777"` is refused by clang and
        // MSVC and truncated-with-a-warning by gcc/mingw. Rejecting it here made
        // the wide form unreachable; the width-aware check is
        // `firstEscapeValueTooWide`, and the narrow path still refuses exactly the
        // same inputs it always did.
        if (e >= '0' && e <= '7') {
            std::uint64_t v = static_cast<std::uint64_t>(e - '0');
            for (int d = 0; d < 2 && i + 1 < body.size()
                            && body[i + 1] >= '0' && body[i + 1] <= '7'; ++d) {
                v = v * 8 + static_cast<std::uint64_t>(body[++i] - '0');
            }
            recordEscape(v, /*hex=*/false);
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
                // `\xH…` — an UNBOUNDED run of hex digits (C 6.4.4.4p7). At least
                // one digit is required; a `\x` followed by a non-hex byte is
                // malformed, never a bare 'x'.
                if (i + 1 >= body.size() || hexVal(body[i + 1]) < 0)
                    return fail(EscapeDecodeError::Malformed);
                std::uint64_t v      = 0;
                int           digits = 0;   // SIGNIFICANT digits — leading zeros are free
                while (i + 1 < body.size() && hexVal(body[i + 1]) >= 0) {
                    // 16 significant hex digits fill a 64-bit value exactly; a
                    // 17th cannot be represented in any code unit that exists, so
                    // it stops here rather than silently wrapping. Leading zeros
                    // do not count — `"\x0000000000041"` is 0x41 on all four
                    // references and must stay accepted.
                    if (digits >= 16) return fail(EscapeDecodeError::EscapeValueTooLarge);
                    v = v * 16 + static_cast<std::uint64_t>(hexVal(body[++i]));
                    if (v != 0) ++digits;
                }
                recordEscape(v, /*hex=*/true);
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
                        return fail(EscapeDecodeError::InvalidUniversalName);
                    int const hv = hexVal(body[i + 1]);
                    if (hv < 0)
                        return fail(EscapeDecodeError::InvalidUniversalName);
                    cp = cp * 16 + static_cast<std::uint32_t>(hv);
                    ++i;
                }
                // FF1: reject a UTF-16 surrogate half / a value past U+10FFFF
                // BEFORE the append (the narrow path has no downstream UTF-8
                // validation). C23 6.4.3 relaxed the <0x00A0 basic-character
                // restriction for string/character LITERALS, so that constraint is
                // intentionally NOT enforced here (`A` = "A" is valid).
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                    return fail(EscapeDecodeError::InvalidUniversalName);
                appendUtf8(cp);
                break;
            }
            default:
                return fail(EscapeDecodeError::Malformed);   // unknown escape — fail loud, never guess
        }
    }
    return oc;
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
    EscapeDecodeOutcome oc = decodeEscapedBytes(body, out);
    bool const ok = oc.ok();
    if (outcome) *outcome = std::move(oc);
    if (!ok) return std::nullopt;
    return out;
}

// Decode a DOUBLED-DELIMITER string body (SQL `'…''…'`): a doubled `delimiter`
// is one literal delimiter byte; every other byte passes through. The coalesced
// body the tokenizer captured contains `''` pairs for embedded quotes and no
// lone delimiter — the body's span STOPS before the closing delimiter — so this
// never fails. (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN changed only who OWNS
// those closing bytes: they used to belong to no token at all, and now form a
// `StringEnd` token FOLLOWING the body. The body's own span and text are
// byte-identical either way, which is exactly why this decoder needed no
// change. The prose here previously said "the closer was consumed on mode-pop";
// the invariant held, the explanation had gone stale.)
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

// Decode a NARROW char-literal body to a single codepoint. The body must resolve
// to EXACTLY one byte (ASCII or a single-byte escape); empty, multi-byte, or
// malformed-escape bodies return std::nullopt (caller fails loud). `outcome`
// (when non-null) receives the specific failure reason + the byte-escape flag;
// note `.ok()` can be true while the result is still nullopt (a valid multi-byte
// UCN such as `é` decodes to >1 byte — not a single narrow char).
//
// ★ THE 8-BIT RANGE CHECK LIVES HERE, and only here, because this entry point is
// the NARROW one by construction — every caller (`pp_if_eval`'s `#if 'x'` fold,
// `cst_const_eval`, `cst_to_hir`'s narrow branch) is a byte-element consumer, and
// a wide/UTF constant goes to `decodeWideCharCodepoint` instead. So `'\x101'` and
// `'\777'` fail loud HERE with the escape recoverable from `*outcome` for the
// diagnostic, rather than silently becoming their low byte. ⚠ The check runs
// BEFORE the one-byte test: `'\x101'` decodes to one placeholder byte, so a
// size-only test would pass it through as 0x01.
[[nodiscard]] inline std::optional<std::uint32_t>
decodeCharLiteralBody(std::string_view body, EscapeDecodeOutcome* outcome = nullptr) {
    std::string out;
    EscapeDecodeOutcome oc = decodeEscapedBytes(body, out);
    bool const ok        = oc.ok();
    bool const tooWide   = ok && firstEscapeValueTooWide(oc, 8).has_value();
    if (outcome) *outcome = std::move(oc);
    if (!ok || tooWide) return std::nullopt;
    if (out.size() != 1) return std::nullopt;   // empty or multi-char
    return static_cast<std::uint32_t>(static_cast<unsigned char>(out[0]));
}

} // namespace dss
