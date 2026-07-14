/* C23 _BitInt(N>64) — MULTI-LIMB bit-precise integers (D-CSUBSET-BITINT-C2-WIDE), C2.
 *
 * A `_BitInt(N>64)` is a ceil(N/64)-limb little-endian value living in memory (like a
 * struct), reached BY ADDRESS. This exercises the full C2 easy-op surface on
 * `_BitInt(128)` (2 limbs, InRegisters ABI) and `_BitInt(200)` (4 limbs, by-reference
 * ABI): + - & | ^ ~ << >> (constant AND variable count) == < >, int<->wide and
 * wide->wide conversions, a BY-VALUE wide arg + wide return, a carry/borrow that
 * CROSSES a limb boundary, a SIGNED multi-limb ordered compare (where signed and
 * unsigned DISAGREE), the GENUINE TOP-LIMB MASK on a non-64-aligned width (_BitInt(200),
 * N%64==8 — a bit set past N must read back 0), and a wide->wide WIDENING whose upper-
 * limb SIGN FILL is read above the source width.
 *
 * The exit code is 42 ONLY when the multi-limb machinery is correct. A NON-CARRYING
 * impl loses the >>64 high limb; a NON-MASKING impl keeps the overflowed top bit; a
 * LOW-LIMB-ONLY impl reads 0 for every high-limb probe (incl. the widening sign fill);
 * an UNSIGNED-for-SIGNED compare flips the ordered compare — each changes the exit
 * (RED-ON-DISABLE: the carry, the borrow, the top-limb mask, the signed compare, the
 * widening fill, and the high-limb reads are all load-bearing on the 42).
 *
 * sizeof/_Alignof are const-folded to size_t (x86-64 psABI: _BitInt(128) is 16B align
 * 8, _BitInt(200) is 32B align 8 — ceil(N/64)*8, NOT 16-aligned). */

_Static_assert(sizeof(_BitInt(128))          == 16, "sizeof(_BitInt(128)) == 16");
_Static_assert(_Alignof(_BitInt(128))        ==  8, "_Alignof(_BitInt(128)) == 8");
_Static_assert(sizeof(_BitInt(200))          == 32, "sizeof(_BitInt(200)) == 32");
_Static_assert(sizeof(unsigned _BitInt(129)) == 24, "sizeof(unsigned _BitInt(129)) == 24");

/* CRIT-B: a struct with a PLAIN (non-bit-field) wide `_BitInt` member — its copy must
 * move ALL 16 bytes byte-wise, never a flat-scalar 8-byte field Load. */
struct WideBox { _BitInt(128) x; };

/* A by-value wide arg + wide RETURN in the InRegisters ABI (128b = two GPRs). */
_BitInt(128) add128(_BitInt(128) a, _BitInt(128) b) { return a + b; }

/* A by-value wide arg + wide RETURN in the by-REFERENCE ABI (200b = 32B > 16B → sret
 * + hidden-pointer arg). `x + x + x` also chains two wide adds. */
_BitInt(200) triple200(_BitInt(200) x) { return x + x + x; }

int main(void) {
    /* ── carry ACROSS a limb boundary, through a by-value call ──────────────────
     * (2^64 - 1) + 1 = 2^64: limb0 wraps to 0, the carry lands in limb1. */
    unsigned _BitInt(128) allones_lo = 18446744073709551615ULL;   /* limb0 = ~0 */
    unsigned _BitInt(128) carried    = add128(allones_lo, 1);     /* 2^64 */
    int a = (int)(carried >> 64);                                 /* high limb = 1 */

    /* borrow back across the limb boundary + wide `==`. */
    unsigned _BitInt(128) back = carried - (unsigned _BitInt(128))1;
    int b = (back == allones_lo) ? 4 : 0;                         /* 4 */

    /* ── the GENUINE TOP-LIMB MASK on `<<` (a bit shifted past N must clear) ─────
     * _BitInt(200) has N%64 == 8, so its top limb (limb 3) keeps ONLY bits 192..199 —
     * the mask is NOT a no-op (unlike _BitInt(128), whose N%64==0 top limb is full and
     * whose overflow bit simply falls outside all limbs). bit 199 is the top valid bit;
     * shifting it to bit 200 lands INSIDE the 4-limb storage but ABOVE N, so ONLY
     * maskTopLimb clears it. A non-masking impl leaves bit 200 set → over != 0 → c=0. */
    unsigned _BitInt(200) topbit    = (unsigned _BitInt(200))1 << 199;
    unsigned _BitInt(200) overflow  = topbit << 1;                /* bit 200 → masked 0 */
    int c = (overflow == (unsigned _BitInt(200))0) ? 5 : 0;       /* 5 (top-limb mask load-bearing) */
    int d = (int)(topbit >> 199);                                 /* 1 (top valid bit reads back) */

    /* ── VARIABLE (runtime) shift crossing limbs on _BitInt(200) ────────────────
     * 130 = 2*64 + 2 — a word offset of 2 plus a 2-bit cross-limb carry. */
    int sc = 130;
    unsigned _BitInt(200) shifted = (unsigned _BitInt(200))1 << sc;   /* variable << */
    int e = (int)(shifted >> sc);                                    /* 1 (variable >>) */

    /* ── bitwise & | ^ and ~ (with its unsigned top-limb mask) on _BitInt(200) ── */
    unsigned _BitInt(200) g = 6, h = 3;
    int f1 = (int)(g & h);                                        /* 2 */
    int f2 = (int)(g | h);                                        /* 7 */
    int f3 = (int)(g ^ h);                                        /* 5 */
    /* ~0 over 200 bits = 2^200-1; >> 199 keeps ONLY bit 199 → 1 IF the top limb was
     * masked to its 8 significant bits (a broken ~ leaves bits 200..255 set → >1). */
    int f4 = (int)(~(unsigned _BitInt(200))0 >> 199);            /* 1 */

    /* ── signed: arithmetic right shift (sign fill) + signed compare ──────────── */
    _BitInt(200) neg = -16;
    int gsh = (int)(neg >> 2);                                    /* -4 (arithmetic) */
    /* SIGNED and UNSIGNED DISAGREE here: as signed, -1 < 1 is TRUE (2); an unsigned-
     * compare regression reads -1 as 2^200-1 → 2^200-1 < 1 is FALSE (0). Load-bearing:
     * a signed/unsigned mixup on the top-limb compare flips hcmp to 0. */
    int hcmp = ((_BitInt(200))-1 < (_BitInt(200))1) ? 2 : 0;     /* 2 (signed <; unsigned would be 0) */

    /* by-value by-ref-ABI arg + return + wide add chain. */
    int k = (int)triple200(1);                                   /* 3 */

    /* ── wide -> wide WIDENING (128b → 200b): the UPPER-LIMB SIGN FILL is load-bearing ─
     * w is NEGATIVE, so widening MUST sign-fill the new upper limbs (2,3) with 1s. bit
     * 150 lives ABOVE the 128-bit source width, so it reads the fill: 1 for a correct
     * sign-extend, 0 for a low-limb-only widen (which leaves limbs 2,3 zero). */
    _BitInt(128) w = -8;
    _BitInt(200) widened = (_BitInt(200))w;                       /* 2-limb → 4-limb, sign-filled */
    int j = ((int)(widened >> 150) & 1) ? 8 : 0;                 /* 8 (fill above src width) */

    /* ── CRIT-B: whole-struct copy of a plain wide `_BitInt` member (all 16 bytes) ─ */
    struct WideBox src;
    src.x = ((_BitInt(128))1 << 100) + 5;                        /* bit 100 (limb 1) + 5 */
    struct WideBox dst = src;                                     /* byte-wise 16B copy */
    int m = (int)(dst.x & (_BitInt(128))0xF);                    /* 5 (low nibble) */
    int n = (int)(dst.x >> 100);                                 /* 1 (limb-1 bit survived) */

    /* 1+4+5+1 + 1 + 2+7+5+1 + (-4)+2 + 3 + 8 + 5+1 == 42, exactly. */
    return a + b + c + d + e + f1 + f2 + f3 + f4 + gsh + hcmp + k + j + m + n;
}
