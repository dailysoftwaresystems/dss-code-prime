/* Prefixed character constants — L'…', u'…', U'…', u8'…' — are integer constant
 * expressions in every position C requires one (C 6.4.4.4 + 6.6), and in #if
 * (C 6.10.1p4), where a character constant reads as signed iff its ELEMENT TYPE is:
 * wchar_t for L'…' (the platform's), char16_t/char32_t/unsigned char for u/U/u8
 * (always unsigned), and plain char for the narrow form (P68 round 9,
 * D-C-PREFIXED-CHARACTER-CONSTANT-IS-NOT-A-CONSTANT-EXPRESSION and
 * D-PP-IF-NARROW-CHARACTER-CONSTANT-IGNORES-PLAIN-CHAR-SIGNEDNESS).
 *
 * Each group sets one bit; the exit is 42 only when all six hold, otherwise the
 * bitmask of the groups that did.
 *
 *   1  every ICE position takes every prefix: a static assertion, an array bound,
 *      enumerators, bit-field widths, _Alignas, index designators, case labels
 *   2  #if reads L'…' with wchar_t's sign (the platform's: signed on x86_64 Linux
 *      and macOS, unsigned on aarch64 Linux and Windows)
 *   4  #if reads u'…', U'…' and u8'…' unsigned on every platform
 *   8  #if reads the narrow form with plain char's sign (unsigned on aarch64 Linux)
 *  16  the widest code unit keeps its element type's sign — in #if and in an ICE
 *  32  every value is the same at run time, through volatiles the optimizer
 *      cannot fold
 *
 * MSVC has no u8 character constant in C (error C3691), so the u8 checks stand
 * behind !defined(_MSC_VER); gcc, clang and mingw-w64 have it, which makes it
 * required here.
 */
#include <stddef.h>

/* The references' answers per platform. */
#if defined(_WIN32)
#define WCHAR_SIGNED 0
#define CHAR_SIGNED 1
#elif defined(__aarch64__) && !defined(__APPLE__)
#define WCHAR_SIGNED 0
#define CHAR_SIGNED 0
#else
#define WCHAR_SIGNED 1
#define CHAR_SIGNED 1
#endif

#if defined(_MSC_VER)
#define HAS_U8_CHAR 0
#else
#define HAS_U8_CHAR 1
#endif

/* ── 1: every ICE position ─────────────────────────────────────────────────── */
_Static_assert(L'a' == 97 && u'a' == 97 && U'a' == 97, "static assertion");
int bound[L'a' == 97 && u'b' == 98 && U'c' == 99 ? 3 : -1];
enum { EL = L'a', Eu = u'b', EU = U'c' };
struct Bits { unsigned f : L'\x03'; unsigned g : u'\x02'; unsigned h : U'\x01'; };
_Alignas(u'\x10') static char aligned;
static int designated[4] = { [U'\x02'] = 7, [L'\x01'] = 5, [u'\x03'] = 9 };
#if HAS_U8_CHAR
_Static_assert(u8'a' == 97, "u8 static assertion");
enum { E8 = u8'd' };
#else
enum { E8 = 100 };
#endif

static int caseLabel(int x) {
    switch (x) {
        case L'a': return 1;
        case u'b': return 2;
        case U'c': return 3;
        default: return 0;
    }
}

/* ── 2, 4, 8: the #if readings ─────────────────────────────────────────────── */
#if (L'a' - 98 < 0) == WCHAR_SIGNED
#define G2 2
#else
#define G2 0
#endif

#if !(u'a' - 98 < 0) && !(U'a' - 98 < 0)
#if HAS_U8_CHAR
#if !(u8'a' - 98 < 0)
#define G4 4
#else
#define G4 0
#endif
#else
#define G4 4
#endif
#else
#define G4 0
#endif

#if ('a' - 98 < 0) == CHAR_SIGNED
#define G8 8
#else
#define G8 0
#endif

/* ── 16: the widest code unit ──────────────────────────────────────────────── */
#if WCHAR_SIGNED
#if L'\xffffffff' < 0
#define G16PP 1
#else
#define G16PP 0
#endif
_Static_assert(L'\xffffffff' < 0, "a signed wchar_t reads its top bit as the sign");
#elif defined(_WIN32)
#if L'\xffff' == 65535
#define G16PP 1
#else
#define G16PP 0
#endif
_Static_assert(L'\xffff' == 65535, "a 16-bit unsigned wchar_t");
#else
#if L'\xffffffff' == 4294967295
#define G16PP 1
#else
#define G16PP 0
#endif
_Static_assert(L'\xffffffff' == 4294967295u, "a 32-bit unsigned wchar_t");
#endif

static volatile int vA = 'a';
static volatile int vMinusOne = -1;

int main(void) {
    int bits = G2 | G4 | G8;

    if (sizeof bound == 3 * sizeof(int) && EL == 97 && Eu == 98 && EU == 99 && E8 == 100
        && ((size_t)&aligned & 15u) == 0 && designated[1] == 5 && designated[2] == 7
        && designated[3] == 9 && caseLabel(vA) == 1 && caseLabel(vA + 1) == 2
        && caseLabel(vA + 2) == 3) {
        struct Bits b = { 7, 3, 1 };
        if (b.f == 7 && b.g == 3 && b.h == 1) bits |= 1;
    }

    if (G16PP) {
        wchar_t const w = (wchar_t)vMinusOne;
        if ((w < 0) == WCHAR_SIGNED) bits |= 16;
    }

    {
        int const a = vA;
        if (L'a' == a && u'a' == a && U'a' == a && 'a' == a
#if HAS_U8_CHAR
            && u8'a' == a
#endif
        ) {
            bits |= 32;
        }
    }

    return bits == 63 ? 42 : bits;
}
