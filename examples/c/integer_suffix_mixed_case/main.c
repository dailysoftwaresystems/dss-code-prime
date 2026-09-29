/* EVERY SPELLING C GIVES THE `long` AND `long long` INTEGER SUFFIXES, MIXED CASE
 * INCLUDED (D-C-MIXED-CASE-INTEGER-SUFFIXES-REFUSED, P68 round 11).
 *
 * C 6.4.4.1 builds an integer-suffix from an unsigned-suffix (`u` or `U`) and a
 * long-suffix (`l`, `L`) or long-long-suffix (`ll`, `LL`), in either order, each
 * part cased on its own: 22 spellings. ✔MEASURED 2026-09-24: gcc 13.3.0 and clang
 * 18.1.3 (`-std=c11 -pedantic-errors`), mingw-w64 gcc and MSVC 19.51 (`/std:c17`)
 * each accept all 22 and run this file to 42. DSS refused eight of them — `uL`,
 * `Ul`, `lU`, `Lu`, `uLL`, `Ull`, `llU`, `LLu` — with P_MalformedNumber, because
 * the C language document listed only the single-case combinations. (MSVC's
 * further mixed-case `ll` spellings are `integer_suffix_mixed_case_long_long_msvc`.)
 *
 * Each check returns its own code when a spelling is refused, typed wrong or
 * valued wrong; 42 only if all 22 are read as 6.4.4.1 says, the ladder's next rung
 * included. The values meet a `volatile`, so no fold can pass them vacuously. The
 * `#if` blocks pin phase 4, which reads the same spellings through the same ladder
 * (C 6.10.1p4): the value, and that a `u` makes the literal unsigned. */

#define IS(T, X) _Generic((X), T: 1, default: 0)
/* The ladder's next rung for a value that needs more than 32 bits: `unsigned long`
 * where it is 64 bits wide (LP64), else `unsigned long long` (LLP64). */
#define UL_OR_ULL(X) _Generic((X), unsigned long: sizeof(unsigned long) >= 5, \
                                   unsigned long long: sizeof(unsigned long) < 5, \
                                   default: 0)

#if 5uL != 5 || 5Ul != 5 || 5lU != 5 || 5Lu != 5 || 5uLL != 5 || 5Ull != 5 || 5llU != 5 || 5LLu != 5
#error "a mixed-case suffix read wrong in #if"
#endif
#if 0uL - 1 < 0 || 0Ul - 1 < 0 || 0lU - 1 < 0 || 0Lu - 1 < 0 || 0uLL - 1 < 0 || 0Ull - 1 < 0 || 0llU - 1 < 0 || 0LLu - 1 < 0
#error "a mixed-case unsigned suffix left the literal signed in #if"
#endif

int main(void) {
    volatile unsigned long long five = 5;
    volatile unsigned long long big = 68719476735ULL;   /* 2^36 - 1 */
    volatile unsigned long long max = 18446744073709551615ULL;
    int const ok[] = {
        /* 1-6: the single spellings */
        IS(unsigned int, 5u) && 5u == five,
        IS(unsigned int, 5U) && 5U == five,
        IS(long, 5l) && 5l == five,
        IS(long, 5L) && 5L == five,
        IS(long long, 5ll) && 5ll == five,
        IS(long long, 5LL) && 5LL == five,
        /* 7-14: unsigned long, all eight spellings; 8, 9, 12 and 13 are mixed case */
        IS(unsigned long, 5ul) && 5ul == five,
        IS(unsigned long, 5uL) && 5uL == five,
        IS(unsigned long, 5Ul) && 5Ul == five,
        IS(unsigned long, 5UL) && 5UL == five,
        IS(unsigned long, 5lu) && 5lu == five,
        IS(unsigned long, 5lU) && 5lU == five,
        IS(unsigned long, 5Lu) && 5Lu == five,
        IS(unsigned long, 5LU) && 5LU == five,
        /* 15-22: unsigned long long, all eight spellings; 16, 17, 20 and 21 are mixed case */
        IS(unsigned long long, 5ull) && 5ull == five,
        IS(unsigned long long, 5uLL) && 5uLL == five,
        IS(unsigned long long, 5Ull) && 5Ull == five,
        IS(unsigned long long, 5ULL) && 5ULL == five,
        IS(unsigned long long, 5llu) && 5llu == five,
        IS(unsigned long long, 5llU) && 5llU == five,
        IS(unsigned long long, 5LLu) && 5LLu == five,
        IS(unsigned long long, 5LLU) && 5LLU == five,
        /* 23-26: the unsigned-long ladder's next rung, decimal and hexadecimal */
        UL_OR_ULL(68719476735uL) && UL_OR_ULL(0xFFFFFFFFFuL) && 68719476735uL == big && 0xFFFFFFFFFuL == big,
        UL_OR_ULL(68719476735Ul) && UL_OR_ULL(0xFFFFFFFFFUl) && 68719476735Ul == big && 0xFFFFFFFFFUl == big,
        UL_OR_ULL(68719476735lU) && UL_OR_ULL(0xFFFFFFFFFlU) && 68719476735lU == big && 0xFFFFFFFFFlU == big,
        UL_OR_ULL(68719476735Lu) && UL_OR_ULL(0xFFFFFFFFFLu) && 68719476735Lu == big && 0xFFFFFFFFFLu == big,
        /* 27-28: the largest unsigned long long, in two mixed spellings */
        IS(unsigned long long, 18446744073709551615uLL) && 18446744073709551615uLL == max,
        IS(unsigned long long, 0xFFFFFFFFFFFFFFFFLLu) && 0xFFFFFFFFFFFFFFFFLLu == max,
    };
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; ++i) {
        if (!ok[i]) return (int)i + 1;
    }
    return 42;
}
