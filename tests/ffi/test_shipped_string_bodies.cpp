// The BODIES DSS ships for <string.h>, RUN (P69, lane `cs`).
//
// ═══ WHY THIS FILE EXISTS ═════════════════════════════════════════════════════
//
// Two functions of C23 7.26 have no C library to import them from, so DSS ships their
// source and compiles it into the program:
//
//   * `strndup`         — src/dss-config/runtime/platform/src/strndup.c         (pe)
//   * `memset_explicit` — src/dss-config/runtime/platform/src/memset_explicit.c (every format)
//
// What such a body DOES was seen by one thing only: a program compiled by DSS and RUN on the
// pair that uses the body (`examples/c/shipped_iso_string_and_ctype_functions`) — for
// `strndup`, on a Windows leg and nowhere else. A body that copied past `n`, or stored
// nothing, was green on every other host.
//
// Both bodies are plain ISO C, so here each is compiled by the leg's OWN C compiler and called.
// The two wrapper units beside this file (shipped_body_strndup.c, shipped_body_memset_explicit.c)
// include the shipped file as it is, under a test-only name; nothing in the shipped tree is
// changed or copied, and the include is the build's own dependency on the shipped file. The
// test-only names are declared ONCE (shipped_string_bodies.h), for the wrappers and for these
// cases: a body and a caller that disagreed about a parameter would otherwise still link.
//
// WHAT IT STATES: the ISO behaviour, 7.26.2.7 and 7.26.6.2, case by case.
// WHAT IT DOES NOT SEE: the body as DSS compiles it. That stays the example's.
//
// RED-ON-DISABLE: let strndup's scan forget its bound and the first and third cases name the
// string; let memset_explicit's loop store nothing and its two storing cases name the byte.

#include "shipped_string_bodies.h"   // the two test-only names, declared once for both sides

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {

// A string the shipped body made: it comes from `malloc`, and `free` releases it.
struct Made {
    char* p;
    explicit Made(char* made) : p{made} {}
    ~Made() { std::free(p); }
    Made(Made const&)            = delete;
    Made& operator=(Made const&) = delete;
};

}  // namespace

// ── strndup, 7.26.2.7 ────────────────────────────────────────────────────────────
//
// "creates a string initialized with no more than `size` initial characters of the array
// pointed to by `s` and up to the first null character, whichever comes first, in a space
// allocated as if by a call to malloc" — and the result is always a string.
TEST(ShippedStringBodies, StrndupCopiesAtMostNCharactersAndAlwaysMakesAString) {
    char const source[] = "abcdef";   // six characters
    struct Row {
        std::size_t n;
        char const* want;
    };
    for (Row const& r : {Row{0, ""}, Row{1, "a"}, Row{3, "abc"}, Row{5, "abcde"},   // n < length
                         Row{6, "abcdef"},                                          // n == length
                         Row{7, "abcdef"}, Row{1000, "abcdef"}}) {                  // n > length
        SCOPED_TRACE(r.n);
        Made const made{dss_shipped_body_strndup(source, r.n)};
        ASSERT_NE(made.p, nullptr);
        EXPECT_STREQ(made.p, r.want);
    }
}

TEST(ShippedStringBodies, StrndupStopsAtANullCharacterBeforeN) {
    char const source[] = {'a', 'b', '\0', 'c', 'd', '\0'};
    for (std::size_t const n : {std::size_t{3}, std::size_t{5}, std::size_t{6}}) {
        SCOPED_TRACE(n);
        Made const made{dss_shipped_body_strndup(source, n)};
        ASSERT_NE(made.p, nullptr);
        EXPECT_STREQ(made.p, "ab");
    }
}

// The array `s` points to need hold NO null character within its first n bytes: the function
// reads at most n of them. Four such bytes, and right behind them bytes it must never reach.
TEST(ShippedStringBodies, StrndupReadsNoCharacterAtOrPastN) {
    struct Adjacent {
        char first[4];
        char behind[4];
    };
    Adjacent const two{{'a', 'b', 'c', 'd'}, {'E', 'F', 'G', '\0'}};
    Made const made{dss_shipped_body_strndup(two.first, sizeof two.first)};
    ASSERT_NE(made.p, nullptr);
    EXPECT_STREQ(made.p, "abcd") << "the scan went past n";
}

// The result is an object of its own: writing it leaves the source as it was, and a request
// for no characters still makes a string (the empty one), never the source and never null.
TEST(ShippedStringBodies, StrndupMakesAnObjectOfItsOwn) {
    char source[] = "abc";
    Made const whole{dss_shipped_body_strndup(source, 3)};
    ASSERT_NE(whole.p, nullptr);
    EXPECT_NE(static_cast<void const*>(whole.p), static_cast<void const*>(source));
    whole.p[0] = 'X';
    EXPECT_STREQ(source, "abc");
    EXPECT_STREQ(whole.p, "Xbc");

    Made const none{dss_shipped_body_strndup(source, 0)};
    ASSERT_NE(none.p, nullptr);
    EXPECT_NE(static_cast<void const*>(none.p), static_cast<void const*>(source));
    EXPECT_STREQ(none.p, "");
}

// ── memset_explicit, 7.26.6.2 ────────────────────────────────────────────────────
//
// "copies the value of c (converted to an unsigned char) into each of the first n characters
// of the object pointed to by s ... returns the value of s."
TEST(ShippedStringBodies, MemsetExplicitStoresInExactlyTheFirstNBytesAndReturnsItsArgument) {
    std::array<unsigned char, 16> bytes{};
    bytes.fill(0x11);
    void* const returned = dss_shipped_body_memset_explicit(bytes.data() + 4, 0xA5, 8);
    EXPECT_EQ(returned, static_cast<void*>(bytes.data() + 4));
    for (std::size_t i = 0; i < bytes.size(); ++i)
        EXPECT_EQ(static_cast<unsigned>(bytes[i]), i >= 4 && i < 12 ? 0xA5u : 0x11u) << "byte " << i;
}

TEST(ShippedStringBodies, MemsetExplicitConvertsItsValueToUnsignedChar) {
    struct Row {
        int      value;
        unsigned stored;
    };
    for (Row const& r : {Row{0x141, 0x41u}, Row{-1, 0xFFu}, Row{0x100, 0x00u}, Row{0, 0x00u},
                         Row{0x7F, 0x7Fu}, Row{0x80, 0x80u}}) {
        SCOPED_TRACE(r.value);
        std::array<unsigned char, 4> bytes{};
        bytes.fill(0x11);
        (void)dss_shipped_body_memset_explicit(bytes.data(), r.value, bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i)
            EXPECT_EQ(static_cast<unsigned>(bytes[i]), r.stored) << "byte " << i;
    }
}

TEST(ShippedStringBodies, MemsetExplicitOfNoBytesStoresNothing) {
    std::array<unsigned char, 4> bytes{};
    bytes.fill(0x11);
    void* const returned = dss_shipped_body_memset_explicit(bytes.data(), 0xA5, 0);
    EXPECT_EQ(returned, static_cast<void*>(bytes.data()));
    for (std::size_t i = 0; i < bytes.size(); ++i)
        EXPECT_EQ(static_cast<unsigned>(bytes[i]), 0x11u) << "byte " << i;
}
