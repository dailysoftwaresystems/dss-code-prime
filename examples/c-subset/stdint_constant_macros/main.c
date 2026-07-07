/* c39 (D-FFI-STDINT-CONSTANT-MACROS): the C99 7.18.4 `*_C` integer-constant
 * macros shipped from <stdint.h> (the sqlite UINT64_C 64-bit-bitmask need, 48x).
 * UINT64_C(0x100000000) must be a FULL 64-bit unsigned constant (4294967296 =
 * 2^32); shifting it right 32 gives 1. RED-ON-DISABLE: if UINT64_C were missing
 * (pre-c39) this would not COMPILE (S_UndeclaredIdentifier); if the `##ULL`
 * suffix/width were dropped (32-bit TRUNCATION) the low 32 bits (0) would shift
 * to 0 -> the `top` term is 0 -> exit 13, not 42. INT64_C/UINT32_C exercise the
 * signed/unsigned-32 family too. 1*29 + 5 + 7 + 1 = 42. */
#include <stdint.h>

uint64_t g_hi = UINT64_C(0x100000000);   /* 2^32 = 4294967296 — full 64-bit */
int64_t  g_s  = INT64_C(5);              /* signed-64 family */
uint32_t g_u  = UINT32_C(7);             /* unsigned-32 family (`v##U`) */

int main(void) {
    int top = (int)(g_hi >> 32);          /* 1 iff UINT64_C is full 64-bit, else 0 */
    int s   = (int)g_s;                   /* 5 */
    int u   = (int)g_u;                   /* 7 */
    return top * 29 + s + u + 1;          /* 29 + 5 + 7 + 1 = 42 (13 if truncated) */
}
