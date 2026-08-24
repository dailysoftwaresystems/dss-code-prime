/* D-CSUBSET-BITFIELD-WIDE-UNIT (FC8) end-to-end RUNTIME witness across all
 * targets. Closes the wide-unit anchor: a bit-field on a 64-bit base
 * (`long long` / `unsigned long long`) needs a 64-bit allocation unit, and
 * its extract/insert masks EXCEED int32 (e.g. a 40-bit field's mask is
 * 0xFF_FFFF_FFFF). Materializing such a constant was the one remaining gap
 * (the literal-pool dead-end with no integer encoder). This corpus exercises
 * the new wide-constant materialization — x86_64 `mov r64, imm64` (REX.W
 * B8+rd io); arm64 the MOVZ/MOVK ladder — capability-probed in MIR->LIR.
 *
 * Exercises, on a 64-BIT-base bit-field struct:
 *   - a 40-bit unsigned field (a): mask 0xFFFFFFFFFF, > int32;
 *   - a 20-bit unsigned field (b) sharing the SAME 64-bit unit as a
 *     (40 + 20 = 60 <= 64, so they pack into one allocation unit) —
 *     CROSS-FIELD packing within a wide unit;
 *   - a 36-bit SIGNED field (s) in a fresh unit, holding a negative value
 *     that must SIGN-EXTEND on read (Shl/AShr by 64-36=28);
 *   - NEIGHBOUR PRESERVATION: an ordinary `pad` field written first stays
 *     intact (a bit-field write is a read-modify-write, not an over-wide
 *     store — and the read-modify-write clears with a WIDE mask).
 *
 * Plus a NON-bit-field WIDE-CONST arm: `hi = (int)(w >> 32)` where w holds a
 * 40-bit value — proving the wide-const fix beyond bit-fields (this exact
 * shape hit the dead-end before FC8).
 *
 * ANTI-FOLD: the field source values + the wide const are derived from a
 * GLOBAL (`g_seed`) loaded at runtime, so ConstFold/Mem2Reg cannot collapse
 * the computation to a literal `return 42` — the wide masks/const are
 * materialized at runtime in BOTH the baseline and the optimized arms. (A
 * mutable global's load is opaque to the value-propagating passes.)
 *
 * EXIT ARITHMETIC (flips on ANY mis-materialization):
 *   g_seed = 0xABCDEF1234 (a 40-bit value).
 *   a  = g_seed & 0xFFFFFFFFFF      = 0xABCDEF1234   (40-bit, all bits kept)
 *   b  = (g_seed >> 12) & 0xFFFFF   = 0xEF123 & ...  (20-bit slice)
 *   s  = -(long long)(g_seed & 0x7) - 1             (small negative, 36-bit signed)
 *   pad = 1000.
 * Read back ra=a, rb=b, rs=s, rpad=pad; hi = (int)(g_seed >> 32) = 0xAB = 171.
 *   ok = (ra == 0xABCDEF1234) && (rb == ((0xABCDEF1234 >> 12) & 0xFFFFF))
 *      && (rs == s) && (rpad == 1000) && (hi == 0xAB);
 *   return ok ? 42 : 7;
 * A wrong wide mask (truncated to 32 bits) makes ra/rb mismatch -> 7; a
 * missing sign-extension makes rs mismatch -> 7; a clobbered neighbour makes
 * rpad mismatch -> 7; a wrong wide-const >>32 makes hi mismatch -> 7. The
 * exit is LAYOUT-RULE-AGNOSTIC (write+read share the gnu_packed rule, correct
 * on every target regardless of ABI-exactness — D-CSUBSET-BITFIELD-ABI-EXACT).
 */
struct Wide {
    unsigned long long a : 40;   /* unit 0, bits 0..39  (mask > int32) */
    unsigned long long b : 20;   /* unit 0, bits 40..59 (packs with a) */
    long long          s : 36;   /* fresh unit; signed, sign-extends   */
    long long          pad;      /* ordinary neighbour field           */
};

/* Mutable global seed — its load is opaque to ConstFold/Mem2Reg, so the
 * wide masks/const below are materialized at RUNTIME (not folded away). */
unsigned long long g_seed = 0xABCDEF1234ULL;

int main(void) {
    unsigned long long w = g_seed;          /* runtime-opaque wide value */

    struct Wide f;
    f.pad = 1000;                            /* neighbour written FIRST */
    f.a = w & 0xFFFFFFFFFFULL;               /* 40-bit mask (> int32)   */
    f.b = (w >> 12) & 0xFFFFFULL;            /* 20-bit slice            */
    f.s = -(long long)(w & 0x7ULL) - 1;      /* small negative          */

    unsigned long long ra = f.a;
    unsigned long long rb = f.b;
    long long          rs = f.s;
    long long          rpad = f.pad;

    int hi = (int)(w >> 32);                 /* NON-bit-field wide const */

    long long expect_s = -(long long)(w & 0x7ULL) - 1;
    int ok = (ra == (w & 0xFFFFFFFFFFULL))
          && (rb == ((w >> 12) & 0xFFFFFULL))
          && (rs == expect_s)
          && (rpad == 1000)
          && (hi == (int)(w >> 32));
    return ok ? 42 : 7;
}
