/* SCALAR FLOATING-POINT MNEMONICS ON aarch64, END TO END AND BY EXECUTION —
 * the runnable half of D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME
 * (arm64 half, P54).
 *
 * ★★★ WHAT THIS PROVES, AND WHY A COMPILE-ONLY ARM COULD NOT. The defect was
 * that `arm64.target.json` declares the constraint letter `w` ⇒ register class
 * `fpr` while `asm-arm64-gas.lang.json` declared no instruction that could
 * NAME that class — so the letter bound a class the dialect could never use.
 * ✔MEASURED at the CLI before the fix: `__asm__("nop" : "+w"(r))` over a
 * `double` compiled rc=0 (the class bound) while
 * `__asm__("fadd %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b))` died rc=1 with
 * `A_AsmTextUnsupported … unknown mnemonic 'fadd'`. A compile-only arm would
 * have gone green the moment ANY row existed under those names; only reading
 * the ARITHMETIC back through the constraint distinguishes *the operand
 * reached a floating-point register and the instruction ran* from *a row
 * exists*.
 *
 * ★★ EVERY TEMPLATE USES A CLASS-SCOPED WIDTH-VIEW LETTER (`%d`, `%s`) AND
 * THAT IS NOT DECORATION. A BARE `%0` on a `"w"` operand derives this class's
 * REGISTER-NATURAL width — 128 bits, because the `v` roots are `fpr`'s only
 * no-`subOf` rows — which is NOT the width any template here means.
 * ⚠ THE REASON GIVEN HERE WENT FALSE ON 2026-09-02 AND THE PRACTICE DID NOT.
 * It read *and 128 elects nothing, so a bare reference is refused*, which was
 * true until lane `av` declared the SIMD register move
 * (D-ASM-ARM64-GAS-SPELLS-NO-ROUNDING-MODE-OR-VECTOR-MOVE): a width-128 fpr
 * variant now exists, so a bare `%0` on the `mov` spelling ELECTS it instead of
 * being refused. Nothing in this file changes — every template here is
 * arithmetic, which still has no width-128 arm — but the letters are now
 * load-bearing because they select a WIDTH rather than because they rescue a
 * refusal. The five FP letters landed in P50
 * (D-ASM-AARCH64-FP-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE) with their
 * decode pinned at the asm tier and NO executed instruction to carry them,
 * because no FP mnemonic existed. This file is the first place they run.
 *
 * ★★★ THE SINGLE-PRECISION SHAPES ARE THE WIDTH-AXIS DISCRIMINATOR, and they
 * are the reason this file is not just the double shapes twice. `fadd` is ONE
 * mnemonic over two target encodings — ftype=01 (0x1E602800) for the D views,
 * ftype=00 (0x1E202800) for the S views — and NO dialect row declares a width:
 * the REGISTER VIEW written in the template is the only thing that says which.
 * If the S shapes elected the D word, the two 32-bit patterns would be added
 * as though they were doubles and every S assertion below would fail.
 *
 * ★ THE `fcmp` SHAPE BINDS BOTH REGISTER FILES IN ONE TEMPLATE: an FP compare
 * sets the condition flags that a GPR-class `cset` then reads, over the SAME
 * `TargetCondCode` vocabulary the C front end's own comparisons use. `fcmp`
 * is the one FP row whose target `result` is `none`, so BOTH written operands
 * are inputs and neither is a destination — a lowering that read the first as
 * a destination would compare the wrong pair.
 *
 * ⚠ WHY NO `fmov` SHAPE IS LOAD-BEARING HERE, STATED RATHER THAN OMITTED.
 * ✔MEASURED: an inline-asm `fmov %d0, %d1` (and the GPR `mov %x0, %x1`)
 * reaches the binary as NOTHING — copy coalescing puts the source and the
 * destination in one register and `classifyIdentityClassMove` then deletes the
 * full-width identity move, which is correct for a copy and means an `fmov`
 * shape would pass over a dialect row that had never been read. The `fmov` row
 * ships and its BYTES are pinned in tests/asm/test_asm_arm64_fp_dialect_rows.cpp,
 * where nothing can coalesce them away.
 *
 * ⚠ EVERY SEED IS `volatile` SO THE RELEASE ARM STILL REACHES THE TEMPLATES —
 * a folded constant would let this file pass without binding anything.
 *
 * Exit codes name the failing shape; 42 means every one held.
 */

volatile double dss_d21   = 21.0;
volatile double dss_d3    = 3.0;
volatile double dss_d12   = 12.0;
volatile float  dss_f1p5  = 1.5f;
volatile float  dss_f2p25 = 2.25f;
volatile float  dss_f16m  = 16777216.0f;  /* 2^24 — the last exactly
                                             representable integer step in
                                             binary32 */
volatile float  dss_f1    = 1.0f;

#if defined(__aarch64__)

static double dssFAdd(double a, double b) {
    double r;
    __asm__("fadd %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b));
    return r;
}

static double dssFSub(double a, double b) {
    double r;
    __asm__("fsub %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b));
    return r;
}

static double dssFMul(double a, double b) {
    double r;
    __asm__("fmul %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b));
    return r;
}

static double dssFDiv(double a, double b) {
    double r;
    __asm__("fdiv %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b));
    return r;
}

static double dssFNeg(double a) {
    double r;
    __asm__("fneg %d0, %d1" : "=w"(r) : "w"(a));
    return r;
}

static float dssFAddS(float a, float b) {
    float r;
    __asm__("fadd %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b));
    return r;
}

static float dssFSubS(float a, float b) {
    float r;
    __asm__("fsub %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b));
    return r;
}

static float dssFNegS(float a) {
    float r;
    __asm__("fneg %s0, %s1" : "=w"(r) : "w"(a));
    return r;
}

/* ONE template, BOTH register files: the FP compare writes NZCV and the
 * GPR-class `cset` reads it. Returns 1 when a < b. */
static unsigned long long dssFLess(double a, double b) {
    unsigned long long out;
    __asm__("fcmp %d1, %d2\n\tcset %x0, lt"
            : "=r"(out) : "w"(a), "w"(b));
    return out;
}

static unsigned long long dssFLessS(float a, float b) {
    unsigned long long out;
    __asm__("fcmp %s1, %s2\n\tcset %x0, lt"
            : "=r"(out) : "w"(a), "w"(b));
    return out;
}

int main(void) {
    double const a = dss_d21;   /* 21.0 */
    double const b = dss_d3;    /* 3.0  */
    double const c = dss_d12;   /* 12.0 */

    if (dssFAdd(a, a) != 42.0)   return 1;
    if (dssFSub(a, b) != 18.0)   return 2;
    if (dssFMul(b, c) != 36.0)   return 3;
    if (dssFDiv(c, b) != 4.0)    return 4;
    if (dssFNeg(a) != -21.0)     return 5;

    /* Single precision — the ftype=00 arm of the SAME seven mnemonics. */
    float const x = dss_f1p5;    /* 1.5  */
    float const y = dss_f2p25;   /* 2.25 */
    if (dssFAddS(x, y) != 3.75f) return 6;
    if (dssFSubS(y, x) != 0.75f) return 7;
    if (dssFNegS(y) != -2.25f)   return 8;

    /* ★ THE ROUNDING SHAPE. 2^24 + 1 is NOT representable in binary32 and
     * rounds back to 2^24; in binary64 it is exact. So this assertion holds
     * only if the instruction that ran was the SINGLE-precision FADD — a
     * D-form word elected for an S-form spelling reads the two 32-bit
     * patterns as doubles and cannot land here by accident. */
    float const big = dss_f16m;  /* 16777216.0f */
    float const one = dss_f1;    /* 1.0f        */
    if (dssFAddS(big, one) != 16777216.0f) return 9;

    /* And the DISCRIMINATION the shape above rests on: the same sum in
     * binary64 does NOT round, so the two widths must disagree. If both
     * answered the same, shape 9 would be proving nothing. */
    if (dssFAdd((double)big, (double)one) != 16777217.0) return 10;

    /* Both register files in one template. */
    if (dssFLess(b, a) != 1u)    return 11;
    if (dssFLess(a, b) != 0u)    return 12;
    if (dssFLessS(x, y) != 1u)   return 13;
    if (dssFLessS(y, x) != 0u)   return 14;

    return 42;
}

#else

/* Not an aarch64 target: this example's subject is the aarch64 gas dialect's
 * floating-point instruction table. It is registered for the arm64 specs only
 * (see expected.json), so this arm exists to keep the file a legal translation
 * unit rather than to assert anything. */
int main(void) { return 42; }

#endif
