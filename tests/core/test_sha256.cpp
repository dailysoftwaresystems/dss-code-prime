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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

// ═══ LOWERCASE BASE32 — `dss::crypto::toBase32Lower` ═══════════════════════
//
// ★★★ WHY THIS ENCODER'S ALPHABET IS A CORRECTNESS PROPERTY AND NOT A STYLE
// ONE. It renders the PATH INDEX of the shipped runtime's object cache
// ([[D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF]]) — i.e. its output becomes a
// FILENAME. Windows and macOS filesystems are CASE-INSENSITIVE, so an encoder
// whose alphabet contained both cases of a letter (base64, base62, base58)
// would let two DISTINCT digests name ONE file, silently, on two of this
// project's three host platforms and never on the third. The case-folding
// group below is therefore the load-bearing one; the RFC vectors merely prove
// the bit packing.

namespace {

using dss::crypto::toBase32Lower;

// RFC 4648's base32 alphabet, lowercased — written out here, INDEPENDENTLY of
// the implementation's own copy, so an alphabet assertion is not checking the
// subject against itself.
constexpr std::string_view kAlphabet = "abcdefghijklmnopqrstuvwxyz234567";

std::string base32Text(std::string_view text) {
    return toBase32Lower(std::span<std::uint8_t const>{
        reinterpret_cast<std::uint8_t const*>(text.data()), text.size()});
}

std::string asciiFold(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

} // namespace

// RFC 4648 §10's own test vectors, lowercased and with the `=` padding removed
// (this encoder is documented as unpadded). These are an INDEPENDENT ORACLE:
// they come from the RFC, not from running our code. They also sweep every
// partial-group remainder — 1, 2, 3, 4 and 5 bytes — which is where a bit-shift
// encoder actually goes wrong.
TEST(Base32Lower, Rfc4648SectionTenVectors) {
    EXPECT_EQ(base32Text(""), "");
    EXPECT_EQ(base32Text("f"), "my");
    EXPECT_EQ(base32Text("fo"), "mzxq");
    EXPECT_EQ(base32Text("foo"), "mzxw6");
    EXPECT_EQ(base32Text("foob"), "mzxw6yq");
    EXPECT_EQ(base32Text("fooba"), "mzxw6ytb");
    EXPECT_EQ(base32Text("foobar"), "mzxw6ytboi");
}

// ★★★ THE CASE-INSENSITIVITY PROPERTY, PROVED EXHAUSTIVELY RATHER THAN
// ARGUED. Over ALL 65,536 two-byte inputs, no two distinct inputs render
// outputs that are equal after ASCII case folding. That is exactly the
// statement "a case-insensitive filesystem cannot alias two of these names",
// and it is the one a mixed-case alphabet fails: base64 would collide `a` with
// `A` on the very first pair.
TEST(Base32Lower, NoTwoDistinctInputsFoldToTheSameRender) {
    // ⚠ THE TWO CLAIMS ARE ACCUMULATED AND ASSERTED AFTER THE LOOP, NOT INSIDE
    // IT, and that is deliberate: an `ASSERT_` in the sweep would return from
    // the test on the FIRST render that is not its own case-folding, so the
    // COLLISION claim below — the one a mixed-case alphabet actually breaks —
    // would never be evaluated and could never be shown red.
    std::vector<std::string> folded;
    folded.reserve(65536u);
    std::size_t notItsOwnFolding = 0;
    std::string firstOffender;
    for (unsigned hi = 0; hi < 256u; ++hi) {
        for (unsigned lo = 0; lo < 256u; ++lo) {
            std::array<std::uint8_t, 2> const input{
                static_cast<std::uint8_t>(hi), static_cast<std::uint8_t>(lo)};
            std::string const rendered =
                toBase32Lower(std::span<std::uint8_t const>{input});
            if (rendered != asciiFold(rendered)) {
                if (notItsOwnFolding == 0) firstOffender = rendered;
                ++notItsOwnFolding;
            }
            folded.push_back(asciiFold(rendered));
        }
    }
    ASSERT_EQ(folded.size(), 65536u);

    // (a) Every render is ALREADY its own case-folding — the alphabet carries
    // no uppercase. Asserted on the OUTPUT and not only on the alphabet
    // constant, because it is the output that becomes a filename.
    EXPECT_EQ(notItsOwnFolding, 0u)
        << "renders are not case-stable; first offender: " << firstOffender;

    // (b) …and no two DISTINCT inputs fold together. This is the statement "a
    // case-insensitive filesystem cannot alias two of these names", and it is
    // what a mixed-case alphabet breaks: base64 would collide `a` with `A`.
    std::sort(folded.begin(), folded.end());
    EXPECT_EQ(std::adjacent_find(folded.begin(), folded.end()), folded.end())
        << "two distinct inputs render names a case-insensitive filesystem "
           "cannot tell apart — the encoder's alphabet is not case-safe.";
    EXPECT_EQ(static_cast<std::size_t>(
                  std::unique(folded.begin(), folded.end()) - folded.begin()),
              65536u);
}

// The 32 symbols are pairwise distinct UNDER FOLDING — the alphabet-level
// statement of the same property, kept because it is what a future edit would
// break first (swapping in `A-Z` looks harmless read line by line).
TEST(Base32Lower, TheAlphabetIsThirtyTwoFoldDistinctLowercaseSymbols) {
    ASSERT_EQ(kAlphabet.size(), 32u);
    std::vector<char> symbols{kAlphabet.begin(), kAlphabet.end()};
    for (char const c : symbols) {
        EXPECT_FALSE(c >= 'A' && c <= 'Z') << "uppercase symbol: " << c;
        // RFC 4648 base32 excludes `0`, `1` and `8` — a check that accepted
        // `[a-z0-9]` would admit names this encoder can never produce.
        EXPECT_FALSE(c == '0' || c == '1' || c == '8') << c;
    }
    std::sort(symbols.begin(), symbols.end());
    EXPECT_EQ(std::adjacent_find(symbols.begin(), symbols.end()),
              symbols.end());

    // And every character the encoder emits is drawn from it, over a sweep
    // that includes every byte value.
    std::vector<std::uint8_t> every(256u);
    for (unsigned i = 0; i < 256u; ++i) every[i] = static_cast<std::uint8_t>(i);
    std::string const rendered = toBase32Lower(every);
    for (char const c : rendered) {
        EXPECT_NE(kAlphabet.find(c), std::string_view::npos)
            << "the encoder emitted a symbol outside its alphabet: " << c;
    }
}

// TOTAL and STABLE: defined for every length including zero, `ceil(8n/5)`
// characters long, and identical on repeat calls. The length law is what a
// dropped trailing partial group breaks — and dropping it would make two inputs
// differing only in their last bits render identically, which is the aliasing
// the whole encoder is chosen to prevent.
TEST(Base32Lower, IsTotalAndStableWithTheExactLengthLaw) {
    std::vector<std::uint8_t> bytes;
    for (std::size_t n = 0; n <= 64u; ++n) {
        bytes.assign(n, static_cast<std::uint8_t>(0xa5u));
        std::string const first  = toBase32Lower(bytes);
        std::string const second = toBase32Lower(bytes);
        EXPECT_EQ(first, second) << "not deterministic at n=" << n;
        EXPECT_EQ(first.size(), (n * 8u + 4u) / 5u)
            << "wrong render length at n=" << n;
    }

    // The two saturating inputs, at the exact 10-byte width the cache's path
    // index uses: all-zero and all-ones must both render 16 characters, and
    // must not render the same 16.
    std::array<std::uint8_t, 10> const zeros{};
    std::array<std::uint8_t, 10> ones{};
    ones.fill(0xffu);
    std::string const zeroText = toBase32Lower(std::span<std::uint8_t const>{zeros});
    std::string const oneText  = toBase32Lower(std::span<std::uint8_t const>{ones});
    EXPECT_EQ(zeroText, "aaaaaaaaaaaaaaaa");
    EXPECT_EQ(oneText, "7777777777777777");
    EXPECT_EQ(zeroText.size(), 16u);
    EXPECT_EQ(oneText.size(), 16u);

    // A one-bit difference in the LAST byte must still move the render — the
    // trailing-group assertion, at the width that matters.
    std::array<std::uint8_t, 10> nearlyZero{};
    nearlyZero[9] = 1u;
    EXPECT_NE(toBase32Lower(std::span<std::uint8_t const>{nearlyZero}), zeroText);
}

// `sha256OfText` is the primitive `sha256Hex` renders — it must be the SAME
// digest, or the cache's identity (hex) and its path index (base32 of the raw
// bytes) would describe different hashes of one document.
TEST(Base32Lower, Sha256OfTextAgreesWithTheHexRender) {
    for (std::string_view const text : {"", "abc", "the shipped runtime\n"}) {
        EXPECT_EQ(dss::crypto::toHexLower(dss::crypto::sha256OfText(text)),
                  dss::crypto::sha256Hex(text))
            << "text: " << text;
    }
}
