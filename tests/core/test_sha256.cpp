// FIPS 180-4 / NIST known-answer tests for dss::crypto::sha256.
//
// These vectors are the INDEPENDENT ORACLE for the SHA-256 implementation:
// the expected digests are hard-coded literal hex from the FIPS 180-4 worked
// examples and the NIST CAVP byte-test set, NOT derived from our own code.
// Because ad-hoc Mach-O code-signing hashes each page of the image with this
// function, a single wrong byte here would silently corrupt every signature,
// so the multi-block / padding-boundary vectors (the 56-byte and 112-byte
// inputs) are included specifically to exercise the two-block padding path.

#include "core/crypto/sha256.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

using dss::crypto::sha256;

namespace {

// Lowercase-hex render of the 32-byte digest, for readable EXPECT_EQ output.
std::string toHex(std::array<std::uint8_t, 32> const& digest) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint8_t const byte : digest) {
        out.push_back(kHexDigits[byte >> 4]);
        out.push_back(kHexDigits[byte & 0x0f]);
    }
    return out;
}

// Hash an ASCII string by viewing its bytes as a span (no trailing NUL).
std::string hashText(std::string_view text) {
    std::span<std::uint8_t const> bytes{
        reinterpret_cast<std::uint8_t const*>(text.data()), text.size()};
    return toHex(sha256(bytes));
}

} // namespace

// FIPS 180-4 §B.1 / NIST: the empty message.
TEST(Sha256, EmptyString) {
    EXPECT_EQ(hashText(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// FIPS 180-4 §B.1: the canonical "abc" one-block example.
TEST(Sha256, Abc) {
    EXPECT_EQ(hashText("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// FIPS 180-4 §B.2: 56-byte message. This is exactly at the padding boundary
// (remainder 56 > 55), so it forces a SECOND padding block — the case most
// likely to be mis-implemented.
TEST(Sha256, FiftySixByteMessageForcesSecondPaddingBlock) {
    EXPECT_EQ(
        hashText("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// NIST 896-bit (112-byte) two-block message.
TEST(Sha256, OneHundredTwelveByteTwoBlockMessage) {
    EXPECT_EQ(
        hashText("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                 "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
        // NIST 896-bit two-block known answer. Cross-checked against an
        // independent SHA-256 oracle (.NET System.Security.Cryptography);
        // the digest ends ...afee9d1, NOT ...afee9d8.
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}
