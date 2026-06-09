#include "core/crypto/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dss::crypto {

namespace {

// Right-rotate a 32-bit word by n bits (0 < n < 32). Hand-rolled rather than
// std::rotr so the implementation has no dependency on <bit> / C++20 library
// support and is byte-identical across MSVC / GCC / Clang. The shift amounts
// used below are all compile-time constants in (1..31), so the (32 - n) shift
// never hits the UB shift-by-32 / shift-by-0 corners.
[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32u - n));
}

// The 64 SHA-256 round constants K: the first 32 bits of the fractional parts
// of the cube roots of the first 64 primes (FIPS 180-4 §4.2.2).
constexpr std::array<std::uint32_t, 64> kK = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

// Process exactly one 64-byte block into the running hash state H.
void processBlock(std::array<std::uint32_t, 8>& h,
                  std::uint8_t const* block) noexcept {
    // Prepare the 64-word message schedule W.
    std::array<std::uint32_t, 64> w{};

    // First 16 words: the block bytes interpreted big-endian.
    for (std::size_t t = 0; t < 16; ++t) {
        std::size_t const i = t * 4;
        w[t] = (static_cast<std::uint32_t>(block[i]) << 24) |
               (static_cast<std::uint32_t>(block[i + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i + 2]) << 8) |
               (static_cast<std::uint32_t>(block[i + 3]));
    }

    // Remaining 48 words via the σ0/σ1 expansion (FIPS 180-4 §6.2.2 step 1).
    for (std::size_t t = 16; t < 64; ++t) {
        std::uint32_t const s0 =
            rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        std::uint32_t const s1 =
            rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    // Working variables a..h initialised from the current hash value.
    std::uint32_t a = h[0];
    std::uint32_t b = h[1];
    std::uint32_t c = h[2];
    std::uint32_t d = h[3];
    std::uint32_t e = h[4];
    std::uint32_t f = h[5];
    std::uint32_t g = h[6];
    std::uint32_t hh = h[7];

    // The 64-round compression function (FIPS 180-4 §6.2.2 step 3).
    for (std::size_t t = 0; t < 64; ++t) {
        std::uint32_t const bigSigma1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t const ch = (e & f) ^ (~e & g);
        std::uint32_t const t1 = hh + bigSigma1 + ch + kK[t] + w[t];
        std::uint32_t const bigSigma0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t const maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t const t2 = bigSigma0 + maj;

        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    // Add the compressed chunk back into the running hash value.
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

} // namespace

std::array<std::uint8_t, 32> sha256(std::span<std::uint8_t const> data) {
    // The 8 initial hash values: the first 32 bits of the fractional parts of
    // the square roots of the first 8 primes (FIPS 180-4 §5.3.3).
    std::array<std::uint32_t, 8> h = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    std::size_t const len = data.size();

    // Process every whole 64-byte block of the input directly.
    std::size_t offset = 0;
    for (; offset + 64 <= len; offset += 64) {
        processBlock(h, data.data() + offset);
    }

    // Build the final padded tail. The remaining (< 64) input bytes, the 0x80
    // terminator, the zero pad, and the 64-bit big-endian message length must
    // land on a 64-byte boundary. That is one block when the remainder is
    // <= 55 bytes (1 terminator + up to 8 length bytes still fit), otherwise
    // two blocks.
    std::array<std::uint8_t, 128> tail{};
    std::size_t const remaining = len - offset;
    for (std::size_t i = 0; i < remaining; ++i) {
        tail[i] = data[offset + i];
    }

    // Append the single '1' bit (FIPS 180-4 §5.1.1: a 0x80 byte).
    tail[remaining] = 0x80;

    std::size_t const tailBlocks = (remaining <= 55) ? 1 : 2;
    std::size_t const tailLen = tailBlocks * 64;

    // Append the message length in BITS as a big-endian 64-bit integer in the
    // final 8 bytes of the tail. (Bytes between the 0x80 and here are already
    // zero from value-initialisation.)
    std::uint64_t const bitLen = static_cast<std::uint64_t>(len) * 8u;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[tailLen - 1 - i] =
            static_cast<std::uint8_t>((bitLen >> (8u * i)) & 0xffu);
    }

    processBlock(h, tail.data());
    if (tailBlocks == 2) {
        processBlock(h, tail.data() + 64);
    }

    // Emit the digest big-endian: H0..H7, most-significant byte first.
    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((h[i] >> 24) & 0xffu);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xffu);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xffu);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xffu);
    }
    return digest;
}

} // namespace dss::crypto
