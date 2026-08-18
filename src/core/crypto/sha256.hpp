#pragma once

// FIPS 180-4 SHA-256.
//
// A self-contained, byte-exact SHA-256 implementation. This is the crypto
// foundation for ad-hoc Mach-O code-signing: the signature blob embeds a
// SHA-256 hash of every page of the image (and of the Code Directory
// itself), so the whole codesign feature's correctness rests on this being
// bit-for-bit identical to the reference algorithm. It is therefore pinned
// against the canonical NIST test vectors (see tests/core/test_sha256.cpp).
//
// No platform crypto API is used — the algorithm is implemented directly so
// the digest is identical on every host (MSVC / GCC-13 / Clang-19) and so a
// macOS-targeting build can run on a Linux or Windows host without depending
// on the host's libcrypto.
//
// Includes are explicit (no reliance on transitive headers): std::array,
// fixed-width integers, std::span, and std::byte-adjacent size types, plus
// std::string / std::string_view for the render helpers below.
//
// ── THIS FILE IS ALSO THE ONE HOME FOR RENDERING DIGEST BYTES AS TEXT ───────
// `toHexLower` and `toBase32Lower` are byte→text encoders rather than hashing,
// and they live here on purpose: they exist only to render a digest, every
// caller reaches them through this header already, and splitting "render a
// digest" across two headers is how a third open-coded nibble loop gets
// written. The alphabet each one commits to is decided ONCE, here.

#include "core/export.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace dss::crypto {

// One-shot SHA-256 over a byte span. Returns the 32-byte digest in the
// canonical big-endian order produced by FIPS 180-4 (digest[0] is the most
// significant byte of H0).
[[nodiscard]] DSS_EXPORT std::array<std::uint8_t, 32>
sha256(std::span<std::uint8_t const> data);

// Lowercase hex render of a byte span — two `[0-9a-f]` chars per byte, most
// significant nibble first. THE repo's hex encoder for digests: before this
// existed the only two spellings were `windows_command_line.hpp`'s UPPERCASE
// `%`-escape table (a different concern, and the wrong case) and a file-local
// copy inside `tests/core/test_sha256.cpp`. Anything rendering a digest calls
// this rather than open-coding a third nibble loop.
[[nodiscard]] DSS_EXPORT std::string toHexLower(std::span<std::uint8_t const> bytes);

// ── LOWERCASE BASE32, RFC 4648's ALPHABET, NO PADDING ───────────────────────
//
// `abcdefghijklmnopqrstuvwxyz234567` — RFC 4648 §6's alphabet, lowercased.
// 5 bits per character, most significant bits first; the final partial group
// is zero-extended and emitted, so the render is TOTAL over any byte span
// (including an empty one) and its length is always `ceil(bytes*8/5)`.
//
// ★★★ THE ALPHABET IS A CORRECTNESS CONSTRAINT, NOT A TASTE ONE, AND THE
// REASON IS FILENAMES. Windows and macOS filesystems are CASE-INSENSITIVE. Any
// encoder whose alphabet contains BOTH cases of a letter — base64, base62,
// base58 — collapses under that folding, so two DISTINCT digests can name ONE
// file. A content-addressed cache built on such a name has a silent aliasing
// bug on two of this project's three host platforms. Lowercase base32 is the
// densest case-insensitive-safe encoding there is: 5 bits per character against
// hex's 4, and every symbol is its own case-folding.
// ⛔ DO NOT "improve" this to base64 or base62. That is the defect, not an
// optimization, and it is invisible on Linux.
//
// ⓘ NO `=` PADDING. RFC 4648 §3.2 makes padding optional when the length is
// known by other means, and every caller here feeds a fixed-length digest
// prefix. `=` is also the one alphabet-adjacent character that is awkward in a
// shell, which a filename should not be.
[[nodiscard]] DSS_EXPORT std::string
toBase32Lower(std::span<std::uint8_t const> bytes);

// The RAW digest of text held as a string_view — the primitive `sha256Hex`
// renders. Exposed rather than left private because a content-addressed
// consumer needs TWO renderings of ONE digest (a hex identity and a short
// base32 path index), and re-deriving bytes from the hex string would be a
// second decoder that could disagree with this encoder. Hash once, render
// twice.
[[nodiscard]] DSS_EXPORT std::array<std::uint8_t, 32>
sha256OfText(std::string_view text);

// `toHexLower(sha256(...))` for text held as a string_view — the exact call a
// content-addressed consumer makes. Hashes the EXACT bytes of `text`: no
// encoding conversion, no trailing NUL, no newline normalisation. The
// reinterpret_cast from `char const*` to `std::uint8_t const*` lives in
// `sha256OfText`, once, instead of at every call site.
[[nodiscard]] DSS_EXPORT std::string sha256Hex(std::string_view text);

} // namespace dss::crypto
