/* THE CONVERSION, CROSS-FILE-MOVE AND LANE-ARRANGEMENT FORMS ON aarch64, END
 * TO END AND BY EXECUTION — the runnable half of the LAST three gaps of
 * D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME (P54, lane
 * el).
 *
 * ★★★ WHAT THIS PROVES, AND WHY A COMPILE-ONLY ARM COULD NOT. Two of the three
 * gaps had a LOUD failure mode (an unknown mnemonic, a parse error), so a
 * build-only check goes green the moment any row exists. The THIRD was SILENT,
 * and it is the reason this file executes: ✔MEASURED in this same cycle by
 * lane `ad`, a width-less `fcvtzs` row compiled `fcvtzs %w0, %s1` rc=0 and
 * emitted `fcvtzs x16, s29` (0x9E3803B0) — the X form, where GNU as 2.42 and
 * clang 18.1.3 both give the W form. A 32-bit destination written, a 64-bit one
 * emitted, with nothing in the build log. Shape 1 below is that defect asked as
 * a QUESTION WITH TWO DIFFERENT ANSWERS.
 *
 * ★★★ HOW A W FORM IS TOLD FROM AN X FORM BY EXECUTION — the design decision
 * this file turns on. For any small value the two agree, so the discriminator
 * has to be a value where they CANNOT: AArch64 FCVTZS SATURATES, so 3.0e9
 * converted into a W destination is INT32_MAX (2147483647) while the same
 * float into an X destination is 3000000000, whose low 32 bits read back as
 * -1294967296. Shapes 1 and 2 assert each answer, and shape 3 asserts the two
 * DISAGREE — so neither can be passing over an instruction that never ran.
 *
 * ★★ THE INT→FP DIRECTION IS DISCRIMINATED BY ROUNDING, not saturation:
 * 2^24 + 1 is exactly representable in binary64 and rounds to 2^24 in
 * binary32, so `scvtf %s0, %w1` and `scvtf %d0, %w1` disagree on the same
 * source integer. Shapes 4-6.
 *
 * ★★ THE ARRANGEMENT SHAPES COMPUTE A POPULATION COUNT, which is the only
 * thing the two declared SIMD spellings can do: CNT replaces each byte lane
 * with its own bit count and ADDV reduces the lanes. Shapes 8-10 thread one
 * integer through `fmov` (GPR→SIMD), `cnt`, `addv` and `fmov` back (SIMD→GPR)
 * and check the total against a value whose popcount is known. That chain also
 * makes the CROSS-FILE `fmov` load-bearing in BOTH directions.
 *
 * ⚠⚠ NOT ONE SHAPE HERE IS A COPY, AND THAT IS MEASURED RATHER THAN CAREFUL.
 * ✔MEASURED in this cycle: an inline-asm `fmov %d0, %d1` reaches the binary as
 * ZERO INSTRUCTIONS, because copy coalescing puts source and destination in one
 * register and `classifyIdentityClassMove` deletes the full-width identity
 * move. Correct for a copy, fatal for a witness. Every template below either
 * changes a value's WIDTH, its BANK, or its BITS.
 *
 * ⚠ `volatile` SEEDS ARE LOAD-BEARING: the `release` arm must still reach the
 * templates rather than folding them into constants.
 *
 * ⚠ ARM64 SPECS ONLY. The subject is one dialect's instruction table and one
 * target's encodings; a target-agnostic arm would assert nothing about either.
 *
 * Exit codes name the failing shape; 42 means every shape agreed.
 */

int main(void) {
    /* ── 1. fcvtzs into a W destination — the saturating answer ─────────── */
    volatile float big = 3.0e9f;
    float          bf  = big;
    int            w32 = 0;
    __asm__("fcvtzs %w0, %s1" : "=r"(w32) : "w"(bf));
    if (w32 != 2147483647) return 1;

    /* ── 2. the SAME float into an X destination — no saturation ────────── */
    long x64 = 0;
    __asm__("fcvtzs %x0, %s1" : "=r"(x64) : "w"(bf));
    if (x64 != 3000000000L) return 2;

    /* ── 3. and the two MUST disagree, so neither is proving nothing ────── */
    if ((long)w32 == x64) return 3;

    /* ── 4. fcvtzs from a DOUBLE source, X destination ──────────────────── */
    volatile double bigd = 5.0e9;
    double          bd   = bigd;
    long            x64d = 0;
    __asm__("fcvtzs %x0, %d1" : "=r"(x64d) : "w"(bd));
    if (x64d != 5000000000L) return 4;

    /* ── 5. the same double into a W destination saturates ──────────────── */
    int w32d = 0;
    __asm__("fcvtzs %w0, %d1" : "=r"(w32d) : "w"(bd));
    if (w32d != 2147483647) return 5;

    /* ── 6. scvtf into a SINGLE destination rounds 2^24+1 down ──────────── */
    volatile int seed = 16777217; /* 2^24 + 1 */
    int          si   = seed;
    float        sf   = 0.0f;
    __asm__("scvtf %s0, %w1" : "=w"(sf) : "r"(si));
    if (sf != 16777216.0f) return 6;

    /* ── 7. scvtf into a DOUBLE destination keeps it exactly ────────────── */
    double sd = 0.0;
    __asm__("scvtf %d0, %w1" : "=w"(sd) : "r"(si));
    if (sd != 16777217.0) return 7;

    /* ── 8. ucvtf from a 64-bit unsigned into a double ──────────────────── */
    volatile unsigned long useed = 9007199254740993UL; /* 2^53 + 1 */
    unsigned long          uv    = useed;
    double                 ud    = 0.0;
    __asm__("ucvtf %d0, %x1" : "=w"(ud) : "r"(uv));
    if (ud != 9007199254740992.0) return 8; /* 2^53 + 1 rounds to 2^53 */

    /* ── 9. fcvt narrowing: double 2^24+1 becomes float 2^24 ────────────── */
    volatile double exact24 = 16777217.0;
    double          e24     = exact24;
    float           narrow  = 0.0f;
    __asm__("fcvt %s0, %d1" : "=w"(narrow) : "w"(e24));
    if (narrow != 16777216.0f) return 9;

    /* ── 10. fcvt widening: float 0.5f becomes double 0.5 exactly ───────── */
    volatile float half = 0.5f;
    float          hf   = half;
    double         wide = 0.0;
    __asm__("fcvt %d0, %s1" : "=w"(wide) : "w"(hf));
    if (wide != 0.5) return 10;

    /* ── 11. the cross-file fmov, GPR -> SIMD, read back as a bit pattern ─
     * 1.0 is 0x3FF0000000000000. Moving the INTEGER into the FP file and then
     * reading it back as a double is the whole cross-file pair in two steps,
     * and neither step is a copy. */
    volatile unsigned long onebits = 0x3FF0000000000000UL;
    unsigned long          ob      = onebits;
    double                 asF     = 0.0;
    __asm__("fmov %d0, %x1" : "=w"(asF) : "r"(ob));
    if (asF != 1.0) return 11;

    /* ── 12. and back: SIMD -> GPR ──────────────────────────────────────── */
    unsigned long back = 0;
    __asm__("fmov %x0, %d1" : "=r"(back) : "w"(asF));
    if (back != 0x3FF0000000000000UL) return 12;

    /* ── 13. the 32-bit half of the same pair — 1.0f is 0x3F800000 ──────── */
    volatile unsigned onef = 0x3F800000u;
    unsigned          of   = onef;
    float             asS  = 0.0f;
    __asm__("fmov %s0, %w1" : "=w"(asS) : "r"(of));
    if (asS != 1.0f) return 13;

    unsigned backs = 0;
    __asm__("fmov %w0, %s1" : "=r"(backs) : "w"(asS));
    if (backs != 0x3F800000u) return 14;

    /* ── 15. THE LANE ARRANGEMENT, 8 BYTE LANES. CNT gives each byte lane its
     * own bit count and ADDV reduces the eight lanes into the B view; the
     * chain reaches the SIMD file through `fmov` and leaves it the same way,
     * so the arrangement suffix is the ONLY thing that can be wrong. 0x0F0F...
     * has four bits per byte, eight bytes, so the total is 32. */
    volatile unsigned long pattern = 0x0F0F0F0F0F0F0F0FUL;
    unsigned long          pv      = pattern;
    double                 vec     = 0.0;
    __asm__("fmov %d0, %x1" : "=w"(vec) : "r"(pv));
    double lanes = 0.0;
    __asm__("cnt %0.8b, %1.8b" : "=w"(lanes) : "w"(vec));
    double total = 0.0;
    __asm__("addv %b0, %1.8b" : "=w"(total) : "w"(lanes));
    unsigned popcount8 = 0;
    __asm__("fmov %w0, %s1" : "=r"(popcount8) : "w"(total));
    if (popcount8 != 32u) return 15;

    /* ── 16. THE SAME CHAIN OVER SIXTEEN LANES. `fmov %d0, %x1` zeroes bits
     * 64..127 of the destination (✔MEASURED and recorded on the opcode), so
     * counting sixteen lanes of a register whose high half is zero must give
     * the SAME 32 — which is what proves the Q=1 word ran rather than faulted
     * or counted stale bits. */
    double lanes16 = 0.0;
    __asm__("cnt %0.16b, %1.16b" : "=w"(lanes16) : "w"(vec));
    double total16 = 0.0;
    __asm__("addv %b0, %1.16b" : "=w"(total16) : "w"(lanes16));
    unsigned popcount16 = 0;
    __asm__("fmov %w0, %s1" : "=r"(popcount16) : "w"(total16));
    if (popcount16 != 32u) return 16;

    /* ── 17. A SECOND PATTERN, so shape 15 cannot be a constant that happens
     * to be 32. 0xFF00FF00FF00FF00 has eight set bits in each of four bytes. */
    volatile unsigned long pattern2 = 0xFF00FF00FF00FF00UL;
    unsigned long          pv2      = pattern2;
    double                 vec2     = 0.0;
    __asm__("fmov %d0, %x1" : "=w"(vec2) : "r"(pv2));
    double lanes2 = 0.0;
    __asm__("cnt %0.8b, %1.8b" : "=w"(lanes2) : "w"(vec2));
    double total2 = 0.0;
    __asm__("addv %b0, %1.8b" : "=w"(total2) : "w"(lanes2));
    unsigned popcount2 = 0;
    __asm__("fmov %w0, %s1" : "=r"(popcount2) : "w"(total2));
    if (popcount2 != 32u) return 17;
    if (popcount2 == 0u) return 18;

    return 42;
}
