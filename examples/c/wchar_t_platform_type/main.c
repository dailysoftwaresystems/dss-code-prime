/* wchar_t — the <stddef.h>/<stdlib.h> typedef AND the type of L'…'/L"…" — is the
 * PLATFORM's type on every pair (P68 round 9, D-C-WCHAR-T-IS-SIGNED-ON-ARM64-LINUX).
 *
 * The platform decides it per processor AND operating system: `int` on x86_64
 * Linux and on macOS (both arches), `unsigned int` on aarch64 Linux, `unsigned
 * short` on Windows. DSS used to key it on the object format alone, so on aarch64
 * Linux `(wchar_t)-1 > 0` was false while gcc and clang say true. Each group sets
 * one bit; the exit is 42 only when all six hold, otherwise the bitmask of the
 * groups that did.
 *
 *   1  L'a' has the typedef's type (_Generic)
 *   2  an element of L"ab" has the typedef's type
 *   4  (wchar_t)-1 > 0 is the platform's answer, as a constant expression
 *   8  the same at run time, through a volatile
 *  16  sizeof(wchar_t), sizeof(L'a') and __SIZEOF_WCHAR_T__ agree
 *  32  the widest code unit the platform's wchar_t holds keeps its sign rule
 */
#include <stddef.h>
#include <stdlib.h>   /* declares wchar_t too (C 7.24): the SAME type, or this fails */

/* The references' answer, per platform (gcc/clang/mingw/MSVC/Apple clang). */
#if defined(_WIN32) || (defined(__aarch64__) && !defined(__APPLE__))
#define WCHAR_IS_UNSIGNED 1
#else
#define WCHAR_IS_UNSIGNED 0
#endif

static volatile int vMinusOne = -1;

int main(void) {
    int bits = 0;

    if (_Generic(L'a', wchar_t: 1, default: 0)) bits |= 1;
    if (_Generic(L"ab"[0], wchar_t: 1, default: 0)) bits |= 2;
    if (((wchar_t)-1 > 0) == WCHAR_IS_UNSIGNED) bits |= 4;

    wchar_t const w = (wchar_t)vMinusOne;
    if ((w > 0) == WCHAR_IS_UNSIGNED) bits |= 8;

    if (sizeof(wchar_t) == __SIZEOF_WCHAR_T__ && sizeof(L'a') == sizeof(wchar_t)
        && sizeof(L"ab") == 3 * sizeof(wchar_t)) {
        bits |= 16;
    }

#if __SIZEOF_WCHAR_T__ == 4
    if ((L'\xffffffff' > 0) == WCHAR_IS_UNSIGNED) bits |= 32;
#else
    if (L'\xffff' > 0 && (wchar_t)0xffff == L'\xffff') bits |= 32;
#endif

    return bits == 63 ? 42 : bits;
}
