/* THE EXPLICIT-ROUNDING CONVERSIONS AND THE REST OF THE SCALAR FP VOCABULARY
 * ON aarch64, END TO END AND BY EXECUTION — the runnable half of
 * D-ASM-ARM64-GAS-SPELLS-NO-ROUNDING-MODE-OR-VECTOR-MOVE (P54, lane av).
 *
 * ★★★ WHAT THIS PROVES THAT A BUILD-ONLY ARM COULD NOT, AND IT IS THE WHOLE
 * REASON THE FILE RUNS. Every mnemonic here failed LOUD before this cycle
 * ("unknown mnemonic"), so a compile-only check goes green the moment ANY row
 * exists — including a row bound to the WRONG opcode. `fcvtas`, `fcvtms`,
 * `fcvtns` and `fcvtps` differ from the already-shipped `fcvtzs` in NOTHING but
 * the `rmode` field: bind one to `fp_to_si` and the program compiles, links,
 * runs, and returns a plausible integer that is rounded the wrong way. Lane
 * `el` named that hazard when it measured these bytes and left them undeclared.
 * Shapes 1-16 are that hazard asked as questions WITH DIFFERENT ANSWERS.
 *
 * ★★★ THE DISCRIMINATOR IS A HALF, NOT A VALUE THAT ROUNDS THE SAME WAY UNDER
 * EVERY MODE. This is the design decision the file turns on: at 1.5 every one
 * of `fcvtns`/`fcvtas`/`fcvtps` answers 2, so a test on 1.5 would pass under
 * three different instructions and prove nothing. ✔MEASURED on real aarch64
 * (qemu-aarch64, gcc 13.3.0 AND clang 18.1.3 agreeing), +2.5 and -2.5 separate
 * ALL FOUR modes pairwise:
 *
 *              fcvtns   fcvtas   fcvtms   fcvtps
 *      +2.5:      2        3        2        3
 *      -2.5:     -2       -3       -3       -2
 *
 * Every pair of modes differs in at least one of the two rows, so no two can be
 * folded and still pass. Shapes 9-10 assert the disagreements DIRECTLY, so a
 * build in which all four collapsed onto one opcode fails there even if every
 * individual expectation were somehow met.
 *
 * ★★ THE NaN IS THE ONLY DISCRIMINATOR BETWEEN `fmax` AND `fmaxnm`, which is
 * why shapes 25-28 (and 44) build one out of a bit pattern rather than skipping
 * the distinction: the two instructions agree on every ordinary input and
 * differ only in whether a NaN operand propagates. ✔MEASURED: fmax(NaN,1.0) is
 * NaN and fmaxnm(NaN,1.0) is 1.0, on both references — and the same for
 * fmin/fminnm.
 *
 * ★ ALL 28 SPELLINGS THIS CYCLE DECLARED EXECUTE HERE. That is a checkable
 * claim rather than a flourish: `fmin` was the last one without a shape and it
 * is shape 44, numbered out of order so that codes already recorded elsewhere
 * keep meaning what they meant.
 *
 * ⚠⚠ NOT ONE SHAPE IS A COPY, AND THAT IS MEASURED RATHER THAN CAREFUL.
 * ✔MEASURED earlier in this same cycle: an inline-asm `fmov %d0, %d1` reaches
 * the binary as ZERO INSTRUCTIONS, because copy coalescing puts source and
 * destination in one register and `classifyIdentityClassMove` deletes the
 * identity move. Every template below changes a value's WIDTH, its BANK, or its
 * BITS.
 * ⚠ THE ONE HONEST EXCEPTION IS THE SIMD REGISTER MOVE (shapes 39-40), AND IT
 * IS STATED RATHER THAN DRESSED UP: `mov Vd.16b, Vn.16b` IS a copy, so no
 * execution can distinguish "it ran" from "it was elided" by value alone. What
 * these two shapes prove is that it is LEGAL and LANE-PRESERVING inside a chain
 * that only the vector path can produce. Its BYTES — including that the alias's
 * single written source reaches BOTH the Rn and Rm fields — are pinned in
 * `tests/asm/test_asm_arm64_rounding_dialect_rows.cpp`, with a mutant that
 * deletes the second wire and watches the word change.
 *
 * ⚠ EVERY EXPECTED VALUE BELOW WAS TRANSCRIBED FROM A RUN, not recalled: the
 * same statements were compiled with aarch64-linux-gnu-gcc 13.3.0 AND clang
 * 18.1.3 and executed under qemu-aarch64, and the two agreed on all 43 printed
 * answers. `fnmadd` and `fnmsub` especially — their operand meaning (Rd =
 * -(Rn*Rm) - Ra and Rd = Rn*Rm - Ra) is exactly the sort of fact that is easy
 * to state backwards from memory.
 *
 * ⚠ `volatile` SEEDS ARE LOAD-BEARING: the `release` arm must still reach the
 * templates rather than folding them into constants.
 *
 * ⚠ ARM64 SPECS ONLY. The subject is one dialect's instruction table and one
 * target's encodings.
 *
 * Exit codes name the failing shape; 42 means every shape agreed.
 */

int main(void) {
    volatile double phalf = 2.5, nhalf = -2.5;
    double p = phalf, n = nhalf;
    long   r = 0;

    /* ── 1-4. THE FOUR SIGNED MODES ON +2.5 ─────────────────────────────── */
    __asm__("fcvtns %x0, %d1" : "=r"(r) : "w"(p));
    if (r != 2L) return 1;
    __asm__("fcvtas %x0, %d1" : "=r"(r) : "w"(p));
    if (r != 3L) return 2;
    __asm__("fcvtms %x0, %d1" : "=r"(r) : "w"(p));
    if (r != 2L) return 3;
    __asm__("fcvtps %x0, %d1" : "=r"(r) : "w"(p));
    if (r != 3L) return 4;

    /* ── 5-8. THE SAME FOUR ON -2.5, where the pairs that agreed above now
     * disagree — ties-to-even goes to -2 and ties-away to -3, floor to -3 and
     * ceil to -2. Together with shapes 1-4 this separates all six pairs. ── */
    __asm__("fcvtns %x0, %d1" : "=r"(r) : "w"(n));
    if (r != -2L) return 5;
    __asm__("fcvtas %x0, %d1" : "=r"(r) : "w"(n));
    if (r != -3L) return 6;
    __asm__("fcvtms %x0, %d1" : "=r"(r) : "w"(n));
    if (r != -3L) return 7;
    __asm__("fcvtps %x0, %d1" : "=r"(r) : "w"(n));
    if (r != -2L) return 8;

    /* ── 9-10. AND THE DISAGREEMENTS ASSERTED DIRECTLY, so that four modes
     * folded onto one opcode cannot pass by accident. ─────────────────────── */
    {
        long ns = 0, as = 0, ms = 0, ps = 0;
        __asm__("fcvtns %x0, %d1" : "=r"(ns) : "w"(p));
        __asm__("fcvtas %x0, %d1" : "=r"(as) : "w"(p));
        if (ns == as) return 9;   /* +2.5: 2 vs 3 */
        __asm__("fcvtms %x0, %d1" : "=r"(ms) : "w"(n));
        __asm__("fcvtps %x0, %d1" : "=r"(ps) : "w"(n));
        if (ms == ps) return 10;  /* -2.5: -3 vs -2 */
    }

    /* ── 11-14. THE UNSIGNED HALF. +2.5 separates nu/au and mu/pu. ───────── */
    {
        unsigned long u = 0;
        __asm__("fcvtnu %x0, %d1" : "=r"(u) : "w"(p));
        if (u != 2UL) return 11;
        __asm__("fcvtau %x0, %d1" : "=r"(u) : "w"(p));
        if (u != 3UL) return 12;
        __asm__("fcvtmu %x0, %d1" : "=r"(u) : "w"(p));
        if (u != 2UL) return 13;
        __asm__("fcvtpu %x0, %d1" : "=r"(u) : "w"(p));
        if (u != 3UL) return 14;
    }

    /* ── 15-16. THE NARROW (W) DESTINATION OF TWO OF THE MODES, from a
     * SINGLE source — the axis whose silent failure lane `ad` measured on
     * `fcvtzs` (a W destination written, an X form emitted). ──────────────── */
    {
        volatile float fp = 2.5f, fn = -2.5f;
        float fa = fp, fb = fn;
        int   w = 0;
        __asm__("fcvtas %w0, %s1" : "=r"(w) : "w"(fa));
        if (w != 3) return 15;
        __asm__("fcvtms %w0, %s1" : "=r"(w) : "w"(fb));
        if (w != -3) return 16;
    }

    /* ── 17-23. ROUND-TO-INTEGRAL, float in and float out. The same seven
     * modes, and the same +2.5 / -2.5 discriminator. `frinti` and `frintx`
     * follow whatever FPCR selects, which at entry is round-to-nearest
     * ties-to-even — so they must agree with `frintn` and NOT with `frinta`. */
    {
        double o = 0.0;
        __asm__("frintn %d0, %d1" : "=w"(o) : "w"(p));
        if (o != 2.0) return 17;
        __asm__("frinta %d0, %d1" : "=w"(o) : "w"(p));
        if (o != 3.0) return 18;
        __asm__("frintm %d0, %d1" : "=w"(o) : "w"(n));
        if (o != -3.0) return 19;
        __asm__("frintp %d0, %d1" : "=w"(o) : "w"(n));
        if (o != -2.0) return 20;
        __asm__("frintz %d0, %d1" : "=w"(o) : "w"(n));
        if (o != -2.0) return 21;
        __asm__("frinti %d0, %d1" : "=w"(o) : "w"(p));
        if (o != 2.0) return 22;
        __asm__("frintx %d0, %d1" : "=w"(o) : "w"(p));
        if (o != 2.0) return 23;
    }

    /* ── 24. THE S-FORM OF A ROUNDING MODE, so `ftype` is exercised on this
     * family too and not only on the D form. ─────────────────────────────── */
    {
        volatile float fp = 2.5f;
        float fa = fp, fo = 0.0f;
        __asm__("frintp %s0, %s1" : "=w"(fo) : "w"(fa));
        if (fo != 3.0f) return 24;
    }

    /* ── 25-28. fmax / fmaxnm / fmin / fminnm, DISCRIMINATED BY A NaN. The
     * quiet NaN is built out of a bit pattern and moved across the register
     * banks with the cross-file `fmov`, because C has no way to write one that
     * survives constant folding. ✔MEASURED on both references. ───────────── */
    {
        volatile unsigned long nanbits = 0x7FF8000000000000UL;
        unsigned long nb  = nanbits;
        volatile double onev = 1.0;
        double one = onev, nan = 0.0, o = 0.0;
        __asm__("fmov %d0, %x1" : "=w"(nan) : "r"(nb));
        if (nan == nan) return 25;         /* it really is a NaN */

        __asm__("fmax %d0, %d1, %d2" : "=w"(o) : "w"(nan), "w"(one));
        if (o == o) return 26;             /* fmax PROPAGATES the NaN */
        __asm__("fmaxnm %d0, %d1, %d2" : "=w"(o) : "w"(nan), "w"(one));
        if (o != 1.0) return 27;           /* fmaxnm returns the other operand */
        __asm__("fminnm %d0, %d1, %d2" : "=w"(o) : "w"(nan), "w"(one));
        if (o != 1.0) return 28;
        /* ⚠ SHAPE 44, OUT OF ORDER ON PURPOSE: `fmin` was the one declared
         * spelling with no execution shape, and it is added here rather than
         * renumbered in so the codes already recorded elsewhere keep meaning
         * what they meant. It completes the pair — `fmin` PROPAGATES the NaN
         * where `fminnm` above returns the other operand — so all 28 spellings
         * this cycle declared now run. */
        __asm__("fmin %d0, %d1, %d2" : "=w"(o) : "w"(nan), "w"(one));
        if (o == o) return 44;
    }

    /* ── 29-30. fabs and fsqrt — one clears a bit, one computes. ─────────── */
    {
        volatile double nine = 9.0;
        double n9 = nine, o = 0.0;
        __asm__("fabs %d0, %d1" : "=w"(o) : "w"(n));
        if (o != 2.5) return 29;
        __asm__("fsqrt %d0, %d1" : "=w"(o) : "w"(n9));
        if (o != 3.0) return 30;
    }

    /* ── 31-34. THE FUSED MULTIPLY-ADD SQUARE, whose four corners differ only
     * in two bits and whose operand MEANING is the easy thing to state
     * backwards. ✔MEASURED with (2,3,4): fmadd +10, fmsub -2, fnmadd -10,
     * fnmsub +2 — gcc 13.3.0 and clang 18.1.3 agreeing under qemu. ───────── */
    {
        volatile double m1 = 2.0, m2 = 3.0, ad = 4.0;
        double x = m1, y = m2, z = ad, o = 0.0;
        __asm__("fmadd %d0, %d1, %d2, %d3" : "=w"(o) : "w"(x), "w"(y), "w"(z));
        if (o != 10.0) return 31;
        __asm__("fmsub %d0, %d1, %d2, %d3" : "=w"(o) : "w"(x), "w"(y), "w"(z));
        if (o != -2.0) return 32;
        __asm__("fnmadd %d0, %d1, %d2, %d3" : "=w"(o) : "w"(x), "w"(y), "w"(z));
        if (o != -10.0) return 33;
        __asm__("fnmsub %d0, %d1, %d2, %d3" : "=w"(o) : "w"(x), "w"(y), "w"(z));
        if (o != 2.0) return 34;

        /* 35. fnmul, which is NOT one of the four above. */
        __asm__("fnmul %d0, %d1, %d2" : "=w"(o) : "w"(x), "w"(y));
        if (o != -6.0) return 35;

        /* 36. and the S-form of the FMA, so `ftype` is exercised here too. */
        volatile float fm1 = 2.0f, fm2 = 3.0f, fad = 4.0f;
        float fx = fm1, fy = fm2, fz = fad, fo = 0.0f;
        __asm__("fmadd %s0, %s1, %s2, %s3"
                : "=w"(fo) : "w"(fx), "w"(fy), "w"(fz));
        if (fo != 10.0f) return 36;
    }

    /* ── 37. THE SIGNALLING COMPARE, read through the flags it writes. A
     * two-instruction template: `fcmpe` sets NZCV and `cset` materializes the
     * predicate, so the comparison cannot be optimized into nothing. ─────── */
    {
        long lt = 0;
        __asm__("fcmpe %d1, %d2\n\tcset %x0, lt" : "=r"(lt) : "w"(n), "w"(p));
        if (lt != 1L) return 37;
        __asm__("fcmpe %d1, %d2\n\tcset %x0, lt" : "=r"(lt) : "w"(p), "w"(n));
        if (lt != 0L) return 38;
    }

    /* ── 39-40. THE SIMD REGISTER MOVE, inside a lane chain. `cnt` gives each
     * byte lane its own bit count, the move carries the lanes, `addv` reduces
     * them; 0x0F0F... has four bits per byte over eight bytes, so the total is
     * 32 whether the move used the Q=1 or the Q=0 arm (the source's high half
     * is already zero, because `fmov %d0, %x1` zeroes bits 127:64).
     * ⚠ AS THE BANNER SAYS: this proves the move is LEGAL and LANE-PRESERVING,
     * not that it is irreducible — a register move is a copy and no execution
     * can prove a copy ran. Its bytes are pinned in the unit test. ───────── */
    {
        volatile unsigned long pat = 0x0F0F0F0F0F0F0F0FUL;
        unsigned long pv = pat;
        double vec = 0.0, lanes = 0.0, moved = 0.0, total = 0.0;
        unsigned pc = 0;
        __asm__("fmov %d0, %x1"      : "=w"(vec)   : "r"(pv));
        __asm__("cnt %0.8b, %1.8b"   : "=w"(lanes) : "w"(vec));
        __asm__("mov %0.16b, %1.16b" : "=w"(moved) : "w"(lanes));
        __asm__("addv %b0, %1.8b"    : "=w"(total) : "w"(moved));
        __asm__("fmov %w0, %s1"      : "=r"(pc)    : "w"(total));
        if (pc != 32u) return 39;

        __asm__("mov %0.8b, %1.8b"   : "=w"(moved) : "w"(lanes));
        __asm__("addv %b0, %1.8b"    : "=w"(total) : "w"(moved));
        __asm__("fmov %w0, %s1"      : "=r"(pc)    : "w"(total));
        if (pc != 32u) return 40;

        /* ── 44-45. THE ARRANGEMENTS ONLY **ONE** REFERENCE TAKES, WHICH IS
         * WHY THEY ARE REQUIRED. [[D-ASM-ARRANGEMENT-ERASED-TO-A-WIDTH-BEFORE-ELECTION]],
         * cycle P54 lane `ae`. ✔MEASURED, gas 2.42 and clang 18.1.3 probed
         * SEPARATELY and DISAGREEING — the one disagreement this family has:
         * gas REFUSES `mov v0.4h, v1.4h` and `mov v0.2s, v1.2s`, while clang
         * assembles BOTH to 0x0EA11C20, the same word as `.8b`, because the
         * ORR the alias names is BITWISE and its lane size reaches no bit. One
         * working reference makes a spelling required, so DSS takes them.
         * ⚠ WHAT EXECUTION CAN AND CANNOT WITNESS HERE, stated rather than
         * dressed up: the three spellings emit the SAME word, so no run can
         * tell them apart — what these shapes prove is that the newly-declared
         * arrangements ASSEMBLE and are LANE-PRESERVING end to end, on real
         * silicon, at both configs. The discrimination that matters is the
         * NEGATIVE one (a lane width `cnt` does not read must NOT elect, and a
         * scalar spelling must not reach a lane form), which is a compile-time
         * verdict and is pinned in
         * `tests/asm/test_asm_arm64_conversion_dialect_rows.cpp`. ────────── */
        __asm__("mov %0.4h, %1.4h"   : "=w"(moved) : "w"(lanes));
        __asm__("addv %b0, %1.8b"    : "=w"(total) : "w"(moved));
        __asm__("fmov %w0, %s1"      : "=r"(pc)    : "w"(total));
        if (pc != 32u) return 44;

        __asm__("mov %0.2s, %1.2s"   : "=w"(moved) : "w"(lanes));
        __asm__("addv %b0, %1.8b"    : "=w"(total) : "w"(moved));
        __asm__("fmov %w0, %s1"      : "=r"(pc)    : "w"(total));
        if (pc != 32u) return 45;
    }

    /* ── 41. AND THE CONTROL THAT KEEPS EVERY SHAPE ABOVE HONEST: the
     * already-shipped toward-zero conversion still answers its own way. On
     * -2.5 it gives -2, which is `fcvtms`'s -3 and `fcvtns`'s -2 — so this
     * also re-proves that the NEW floor mode is not the OLD truncation. ──── */
    {
        long z = 0, m = 0;
        __asm__("fcvtzs %x0, %d1" : "=r"(z) : "w"(n));
        if (z != -2L) return 41;
        __asm__("fcvtms %x0, %d1" : "=r"(m) : "w"(n));
        /* ⚠ 43, NOT 42: 42 is this program's SUCCESS code, so a failing shape
         * numbered 42 would be indistinguishable from every shape passing. */
        if (z == m) return 43;
    }

    return 42;
}
