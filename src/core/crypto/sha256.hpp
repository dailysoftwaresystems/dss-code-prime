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
// fixed-width integers, std::span, and std::byte-adjacent size types.

#include "core/export.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dss::crypto {

// One-shot SHA-256 over a byte span. Returns the 32-byte digest in the
// canonical big-endian order produced by FIPS 180-4 (digest[0] is the most
// significant byte of H0).
[[nodiscard]] DSS_EXPORT std::array<std::uint8_t, 32>
sha256(std::span<std::uint8_t const> data);

} // namespace dss::crypto
