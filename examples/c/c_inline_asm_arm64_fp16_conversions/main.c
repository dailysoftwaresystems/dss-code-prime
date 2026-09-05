/* THE HALF-PRECISION `fcvt` FORMS ON aarch64, END TO END AND BY EXECUTION —
 * the runnable half of D-TARGET-ARM64-FP16-CONVERSION-FORMS-UNDECLARED
 * (P55, lane fp).
 *
 * ★★★ WHAT THIS PROVES, AND WHY A COMPILE-ONLY ARM COULD NOT. The gap itself
 * failed LOUD (four spellings both references assemble, all refused by name),
 * so a build-only check goes green the moment any opcode exists. What a build
 * cannot see is whether the four words that now assemble are the four the
 * HARDWARE reads as half-precision conversions. Every one of the six `fcvt`
 * forms differs from its siblings in two 2-bit fields — `type` [23:22] and
 * `opc` [16:15] — so a wrong `type` makes `fcvt h0, d1` read a DOUBLE's low 32
 * bits as a float, and a wrong `opc` makes `fcvt s0, h1` write a double-shaped
 * bit pattern into a register the caller reads as a float. Both compile rc=0.
 *
 * ★★★ HOW HALF PRECISION IS TOLD FROM NO CONVERSION AT ALL — the design
 * decision this file turns on. A round trip through a WIDER format is the
 * identity and would assert nothing; a round trip through a NARROWER one is
 * only observable where the narrowing actually loses something. binary16 keeps
 * 10 explicit mantissa bits, so 1/3 — whose bits repeat forever in every
 * radix-2 format — is the discriminator: 0.3333333432674408f becomes exactly
 * 0.333251953125 and can never become itself again. Shapes 1-6 assert that
 * number and shapes 2, 4 assert the round trip DISAGREES with its input, so no
 * shape can be passing over an instruction that never ran.
 *
 * ★★ THE FOUR FORMS ARE COVERED PAIRWISE, NOT SINGLY, because a half value has
 * no C type to hold it: `_Float16` does not exist in this front end, so every
 * shape narrows and widens inside ONE template. Each form therefore appears in
 * two shapes with a DIFFERENT partner — s→h→s and s→h→d share only `fcvt h,s`;
 * d→h→d and d→h→s share only `fcvt h,d` — so a single wrong word reddens two
 * shapes that have nothing else in common. A partial fix cannot read as a
 * complete one here.
 *
 * ★★ SATURATION AND UNDERFLOW ARE THE SECOND DISCRIMINATOR, and they are the
 * ones a wrong `type` field cannot fake: binary16's largest finite value is
 * 65504, so 1e30 becomes +inf and 1e-30 becomes +0 — answers no other float
 * width gives for those inputs. Shapes 7-9. Shape 10 converts 65504 itself and
 * gets it back EXACTLY, which is what stops shapes 7-9 from being satisfied by
 * an instruction that always saturates or always zeroes.
 *
 * ⚠⚠ NOT ONE SHAPE HERE IS A COPY. ✔MEASURED in P54: an inline-asm
 * `fmov %d0, %d1` reaches the binary as ZERO INSTRUCTIONS, because copy
 * coalescing puts source and destination in one register and
 * `classifyIdentityClassMove` deletes the identity move. Every template below
 * changes the value's WIDTH and its BITS, twice.
 *
 * ⚠ `volatile` SEEDS ARE LOAD-BEARING: the `release` arm must still reach the
 * templates rather than folding them into constants.
 *
 * ⚠ FP16 ARITHMETIC IS DELIBERATELY ABSENT. ✔MEASURED, gas 2.42 and clang
 * 18.1.3 both REFUSE `fadd h0,h1,h2`, `fmov h0,h1` and every other half-width
 * arithmetic form at the default -march (they are `FEAT_FP16`), and so are the
 * fp16 INTEGER conversions `fcvtzs w0,h1` / `scvtf h0,w1`. Only FCVT has half
 * arms in base ARMv8-A; writing any of the others here would be ABOVE the
 * union and the program would not assemble on the references either.
 *
 * ⚠ ARM64 SPECS ONLY. The subject is one target's encodings for one mnemonic.
 *
 * Exit codes name the failing shape; 42 means every shape agreed.
 */

int main(void) {
    /* ── 1. float -> half -> float. 1/3 cannot survive 10 mantissa bits, so
     * the answer is the half-rounded one and nothing else. ─────────────── */
    volatile float third_f = 1.0f / 3.0f; /* 0.3333333432674408f */
    float          tf      = third_f;
    float          r_ss    = 0.0f;
    __asm__("fcvt %h0, %s1\n\tfcvt %s0, %h0" : "=w"(r_ss) : "w"(tf));
    if (r_ss != 0.333251953125f) return 1;

    /* ── 2. and it must DISAGREE with its own input, so shape 1 cannot be
     * passing over a template that emitted nothing. ─────────────────────── */
    if (r_ss == tf) return 2;

    /* ── 3. double -> half -> double, the OTHER narrowing word. A wrong
     * `type` field here would read the double's low 32 bits as a float. ── */
    volatile double third_d = 1.0 / 3.0;
    double          td      = third_d;
    double          r_dd    = 0.0;
    __asm__("fcvt %h0, %d1\n\tfcvt %d0, %h0" : "=w"(r_dd) : "w"(td));
    if (r_dd != 0.333251953125) return 3;

    /* ── 4. and it too must disagree with its input. ─────────────────────── */
    if (r_dd == td) return 4;

    /* ── 5. float -> half -> DOUBLE. Shares only `fcvt h,s` with shape 1 and
     * only `fcvt d,h` with shape 3, so it isolates the pair. ────────────── */
    double r_sd = 0.0;
    __asm__("fcvt %h0, %s1\n\tfcvt %d0, %h0" : "=w"(r_sd) : "w"(tf));
    if (r_sd != 0.333251953125) return 5;

    /* ── 6. double -> half -> FLOAT, the remaining pair. ─────────────────── */
    float r_ds = 0.0f;
    __asm__("fcvt %h0, %d1\n\tfcvt %s0, %h0" : "=w"(r_ds) : "w"(td));
    if (r_ds != 0.333251953125f) return 6;

    /* ── 7. the two NARROWING words must agree about the same number: a
     * float 1/3 and a double 1/3 round to the same binary16. ────────────── */
    if ((double)r_ss != r_dd) return 7;

    /* ── 8. OVERFLOW. binary16's largest finite value is 65504, so 1e30
     * saturates to +inf — an answer no wider format gives for this input.
     * FLT_MAX is about 3.40e38, so only an infinity passes. ─────────────── */
    volatile float huge_f = 1.0e30f;
    float          hf     = huge_f;
    float          r_huge = 0.0f;
    __asm__("fcvt %h0, %s1\n\tfcvt %s0, %h0" : "=w"(r_huge) : "w"(hf));
    if (!(r_huge > 3.0e38f)) return 8;

    /* ── 9. the same through the DOUBLE words. DBL_MAX is about 1.80e308. ── */
    volatile double huge_d = 1.0e30;
    double          hd     = huge_d;
    double          r_hugd = 0.0;
    __asm__("fcvt %h0, %d1\n\tfcvt %d0, %h0" : "=w"(r_hugd) : "w"(hd));
    if (!(r_hugd > 1.0e308)) return 9;

    /* ── 10. UNDERFLOW. binary16's smallest subnormal is about 5.96e-8, so
     * 1e-30 rounds to zero — and a float would have kept it. ────────────── */
    volatile float tiny_f = 1.0e-30f;
    float          yf     = tiny_f;
    float          r_tiny = 1.0f;
    __asm__("fcvt %h0, %s1\n\tfcvt %s0, %h0" : "=w"(r_tiny) : "w"(yf));
    if (r_tiny != 0.0f) return 10;

    /* ── 11. AND THE VALUE THAT MUST SURVIVE, which is what stops 8-10 from
     * being satisfied by an instruction that always saturates or always
     * zeroes: 65504 is exactly representable in binary16. ───────────────── */
    volatile float maxh_f = 65504.0f;
    float          mf     = maxh_f;
    float          r_maxh = 0.0f;
    __asm__("fcvt %h0, %s1\n\tfcvt %s0, %h0" : "=w"(r_maxh) : "w"(mf));
    if (r_maxh != 65504.0f) return 11;

    /* ── 12. the same value through the DOUBLE words, exact both ways. ──── */
    volatile double maxh_d = 65504.0;
    double          md     = maxh_d;
    double          r_maxd = 0.0;
    __asm__("fcvt %h0, %d1\n\tfcvt %d0, %h0" : "=w"(r_maxd) : "w"(md));
    if (r_maxd != 65504.0) return 12;

    /* ── 13. one step past it: 65536 is the next power of two and overflows,
     * so the boundary is where binary16 puts it and not where a wider
     * format would. ────────────────────────────────────────────────────── */
    volatile float over_f = 65536.0f;
    float          of     = over_f;
    float          r_over = 0.0f;
    __asm__("fcvt %h0, %s1\n\tfcvt %s0, %h0" : "=w"(r_over) : "w"(of));
    if (!(r_over > 3.0e38f)) return 13;

    /* ── 14. and 0.5 is exact in every one of the three formats, so the
     * widening words are proved on a value the narrowing cannot alter. ─── */
    volatile float exact_f = 0.5f;
    float          ef      = exact_f;
    double         r_exact = 0.0;
    __asm__("fcvt %h0, %s1\n\tfcvt %d0, %h0" : "=w"(r_exact) : "w"(ef));
    if (r_exact != 0.5) return 14;

    return 42;
}
