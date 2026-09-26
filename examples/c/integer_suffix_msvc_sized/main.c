/* MSVC'S SIZED INTEGER SUFFIXES: `i8` `i16` `i32` `i64` AND `ui8` … `ui64`
 * (D-C-MSVC-SIZED-INTEGER-SUFFIXES-REFUSED, P68 round 13).
 *
 * The suffix FIXES the type — `i8` is plain `char`, `ui8` `unsigned char`, `i16`
 * `short`, `ui16` `unsigned short`, `i32` `int`, `ui32` `unsigned int`, `i64` `long
 * long`, `ui64` `unsigned long long`, the `i` and the `u` each in either case (24
 * spellings) — and a magnitude that type cannot hold is reduced modulo 2^width at
 * the type's signedness. ✔MEASURED 2026-09-25 through `dssharness run
 * probe-reference-cc --legs windows-x86_64-release`, MSVC 19.51.36260: all 24
 * compile with no diagnostic at /W4, `300i8` is 44, `256ui8` is 0,
 * `4294967296i32` is 0, `0xFFFFFFFFFFFFFFFFi64` is -1, in `#if` too (both of
 * MSVC's preprocessors), and MSVC runs this file to 42. gcc 13.3.0, clang 18.1.3
 * and mingw-w64 gcc 13.2.0 refuse all 24 ("invalid suffix"), so MSVC is the only
 * reference that builds it; under `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` one
 * accepting reference makes the spellings required and MSVC is the witness of
 * what they mean.
 *
 * ★ `i8` IS PLAIN `char`, whose signedness is the target's: `0xFFi8` is -1 where
 * `char` is signed (every Windows target) and +255 on aarch64 GNU/Linux. So every
 * `i8` value here is checked against `(char)` of its magnitude, which is the rule
 * itself stated target-free, and the `#if` pin compares against the narrow
 * character constant `'\x80'`, which reads plain `char`'s sign the same way.
 *
 * Each check returns its own code when a spelling is refused, typed wrong or
 * valued wrong; 42 only if all hold. Values meet a `volatile`, so no fold can pass
 * them vacuously; the `#if` blocks pin phase 4. */

#define IS(T, X) _Generic((X), T: 1, default: 0)

#if 300i8 != 44 || 256ui8 != 0 || 4294967296i32 != 0 || 65536i16 != 0
#error "a sized literal was not reduced to its type in #if"
#endif
#if 0xFFFFFFFFFFFFFFFFi64 >= 0 || 2147483648i32 >= 0
#error "a signed sized literal read unsigned in #if"
#endif
#if 5ui8 - 6 < 0 || 5ui16 - 6 < 0 || 5ui32 - 6 < 0 || 5ui64 - 6 < 0 || -1 < 0UI64
#error "an unsigned sized literal read signed in #if"
#endif
#if (128i8 < 0) != ('\x80' < 0)
#error "an i8 literal did not take plain char's signedness in #if"
#endif

_Static_assert(300i8 == 44, "300i8 reduces to the char 44");
_Static_assert(256ui8 == 0 && 0x1FFui8 == 255, "ui8 reduces modulo 256");
_Static_assert(4294967296i32 == 0 && 0xFFFFFFFFFFFFFFFFi64 == -1, "i32 / i64 wrap");
_Static_assert(sizeof(5i8) == 1 && sizeof(5i16) == 2 && sizeof(5i32) == 4
               && sizeof(5i64) == 8, "the four widths");

enum { SIZED_ENUM = 65537ui16 };          /* 1 */
static char sized_bound[300i8];            /* 44 elements */

int main(void) {
    volatile long long five = 5;
    volatile long long zero = 0;
    volatile long long tera = 1099511627776LL;               /* 2^40 */
    int r = 0;
    switch (2i64) {
        case 1i64: r = 1; break;
        case 2i64: r = 2; break;
        default:   r = 3; break;
    }
    int const ok[] = {
        /* 1-24: every spelling at 5, by its TYPE and its VALUE */
        IS(char, 5i8) && 5i8 == five,
        IS(char, 5I8) && 5I8 == five,
        IS(unsigned char, 5ui8) && 5ui8 == five,
        IS(unsigned char, 5Ui8) && 5Ui8 == five,
        IS(unsigned char, 5uI8) && 5uI8 == five,
        IS(unsigned char, 5UI8) && 5UI8 == five,
        IS(short, 5i16) && 5i16 == five,
        IS(short, 5I16) && 5I16 == five,
        IS(unsigned short, 5ui16) && 5ui16 == five,
        IS(unsigned short, 5Ui16) && 5Ui16 == five,
        IS(unsigned short, 5uI16) && 5uI16 == five,
        IS(unsigned short, 5UI16) && 5UI16 == five,
        IS(int, 5i32) && 5i32 == five,
        IS(int, 5I32) && 5I32 == five,
        IS(unsigned int, 5ui32) && 5ui32 == five,
        IS(unsigned int, 5Ui32) && 5Ui32 == five,
        IS(unsigned int, 5uI32) && 5uI32 == five,
        IS(unsigned int, 5UI32) && 5UI32 == five,
        IS(long long, 5i64) && 5i64 == five,
        IS(long long, 5I64) && 5I64 == five,
        IS(unsigned long long, 5ui64) && 5ui64 == five,
        IS(unsigned long long, 5Ui64) && 5Ui64 == five,
        IS(unsigned long long, 5uI64) && 5uI64 == five,
        IS(unsigned long long, 5UI64) && 5UI64 == five,
        /* 25-28: i8 reduced as a plain char — the magnitude converted to char */
        IS(char, 300i8) && 300i8 == 44 + zero,
        IS(char, 0xFFi8) && 0xFFi8 == (char)(0xFF + zero),
        IS(char, 128i8) && 128i8 == (char)(128 + zero),
        IS(int, -128i8) && -128i8 == -(int)(char)(128 + zero),   /* reduced FIRST */
        /* 29-37: every other width, at and past its range edge */
        IS(unsigned char, 256ui8) && 256ui8 == zero,
        IS(unsigned char, 0x1FFui8) && 0x1FFui8 == 255 + zero,
        IS(short, 32768i16) && 32768i16 == -32768 + zero,
        IS(unsigned short, 65536ui16) && 65536ui16 == zero,
        IS(int, 2147483648i32) && 2147483648i32 == -2147483647 - 1 + zero,
        IS(int, 4294967296i32) && 4294967296i32 == zero,
        IS(unsigned int, 4294967296ui32) && 4294967296ui32 == zero,
        IS(long long, 0xFFFFFFFFFFFFFFFFi64) && 0xFFFFFFFFFFFFFFFFi64 == -1 + zero,
        IS(long long, 9223372036854775808i64)
            && 9223372036854775808i64 == -9223372036854775807LL - 1 + zero,
        /* 38-41: constant expressions, and an i64 wider than 32 bits */
        SIZED_ENUM == 1 + zero,
        sizeof sized_bound == 44 + zero,
        r == 2 + zero,
        (1i64 << 40) == tera && 0x10000000000i64 == tera,
    };
    for (int i = 0; i < (int)(sizeof ok / sizeof ok[0]); ++i) {
        if (!ok[i]) return i + 1;
    }
    return 42;
}
