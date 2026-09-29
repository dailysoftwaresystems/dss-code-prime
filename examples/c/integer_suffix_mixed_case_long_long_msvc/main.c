/* MSVC'S MIXED-CASE `ll` SUFFIXES: `lL`, `Ll`, AND THE EIGHT WITH A `u`
 * (D-C-MIXED-CASE-INTEGER-SUFFIXES-REFUSED, P68 round 11).
 *
 * C 6.4.4.1 spells the long-long-suffix `ll` or `LL` only. MSVC also takes the two
 * letters cased differently. ✔MEASURED 2026-09-24, MSVC 19.51 in its default mode,
 * `/std:c17` and `/std:clatest`: `lL`, `Ll`, `ulL`, `uLl`, `UlL`, `ULl`, `lLu`,
 * `lLU`, `Llu` and `LlU` compile without a diagnostic, and each types EXACTLY as
 * its same-case twin (`ll`, or `ull`) at every magnitude measured, decimal and
 * hexadecimal — 2^31, 2^32, 2^63 - 1, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, and
 * the decimal 2^63. gcc 13.3.0, clang 18.1.3 and mingw-w64 gcc refuse all ten
 * ("invalid suffix"), so MSVC is the only reference that builds this file, and it
 * runs it to 42. Under `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` one accepting reference
 * makes the spelling REQUIRED; the MEANING is MSVC's own, a synonym of `ll` / `ull`,
 * so DSS types each through the same 6.4.4.1 rule as its twin. (Where MSVC types
 * `ll` itself away from ISO — it keeps 0x8000000000000000ll `long long` — DSS
 * follows ISO for both, as it already did for `ll`; this file stays off that edge.)
 *
 * Each check returns its own code when a spelling is refused, typed wrong or valued
 * wrong; 42 only if all ten read as their twins. The values meet a `volatile`, so
 * no fold can pass them vacuously, and the `#if` blocks pin phase 4. */

#define IS(T, X) _Generic((X), T: 1, default: 0)

#if 5lL != 5 || 5Ll != 5 || 5ulL != 5 || 5uLl != 5 || 5UlL != 5 || 5ULl != 5 || 5lLu != 5 || 5lLU != 5 || 5Llu != 5 || 5LlU != 5
#error "a mixed-case long long suffix read wrong in #if"
#endif
#if 0ulL - 1 < 0 || 0uLl - 1 < 0 || 0UlL - 1 < 0 || 0ULl - 1 < 0 || 0lLu - 1 < 0 || 0lLU - 1 < 0 || 0Llu - 1 < 0 || 0LlU - 1 < 0
#error "a mixed-case unsigned long long suffix left the literal signed in #if"
#endif

int main(void) {
    volatile unsigned long long five = 5;
    volatile long long tera = 1099511627776LL;           /* 2^40 */
    volatile unsigned long long max = 18446744073709551615ULL;
    int const ok[] = {
        /* 1-2: long long */
        IS(long long, 5lL) && 5lL == five,
        IS(long long, 5Ll) && 5Ll == five,
        /* 3-10: unsigned long long */
        IS(unsigned long long, 5ulL) && 5ulL == five,
        IS(unsigned long long, 5uLl) && 5uLl == five,
        IS(unsigned long long, 5UlL) && 5UlL == five,
        IS(unsigned long long, 5ULl) && 5ULl == five,
        IS(unsigned long long, 5lLu) && 5lLu == five,
        IS(unsigned long long, 5lLU) && 5lLU == five,
        IS(unsigned long long, 5Llu) && 5Llu == five,
        IS(unsigned long long, 5LlU) && 5LlU == five,
        /* 11-12: more than 32 bits, decimal and hexadecimal */
        IS(long long, 1099511627776lL) && 1099511627776lL == tera,
        IS(long long, 0x10000000000Ll) && 0x10000000000Ll == tera,
        /* 13-14: the largest unsigned long long */
        IS(unsigned long long, 18446744073709551615ulL) && 18446744073709551615ulL == max,
        IS(unsigned long long, 0xFFFFFFFFFFFFFFFFLlU) && 0xFFFFFFFFFFFFFFFFLlU == max,
    };
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; ++i) {
        if (!ok[i]) return (int)i + 1;
    }
    return 42;
}
