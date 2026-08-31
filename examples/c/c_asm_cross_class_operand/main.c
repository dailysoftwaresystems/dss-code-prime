/* The CROSS-CLASS register move (D-TARGET-NO-CROSS-CLASS-MOVE-VERB).
 *
 * ★★★ WHAT THIS EXAMPLE IS THE FIRST RUNNABLE WITNESS OF. In GNU inline asm the
 * register class is the CONSTRAINT's, never the VALUE's type's — so `"r"` with
 * a `double` is legal, and the machine has to copy a bit pattern from the
 * floating register FILE into the integer one and back. That is a distinct
 * machine operation from a copy within one file (`fmov x0, d0` on aarch64,
 * `movq %xmm0, %rax` on x86_64), and until this cycle DSS had nowhere to
 * DECLARE it: `registerClassOps` was indexed by ONE class, so the cross-product
 * had no slot. ✔MEASURED 2026-08-31 at the CLI before the fix, every shape in
 * this file was REFUSED on BOTH shipped targets at BOTH configs, rc=1:
 *
 *     error[L_UnsupportedLoweringForOpcode]: … binds register class 'gpr',
 *     but the value it carries lives in class 'fpr'.
 *
 * while `aarch64-linux-gnu-gcc 13.3.0 -O2` compiles the same source to
 * `fmov x0, d0 / nop / fmov d0, x0`. A conformance gap with a working
 * reference — and one whose only alternative, measured under
 * D-LIR-ASM-OPERAND-MOVE-IS-CLASS-BLIND, was a wrong register QUIETLY.
 *
 * ★★★ WHY EVERY OPERAND HERE IS `"+r"` AND WHY THERE ARE THREE OF THEM.
 * A `+` operand is BOTH directions in one place: the value is materialised
 * fpr→gpr on the way in and carried gpr→fpr on the way out, so one shape
 * exercises both off-diagonal cells of the table. And the COUNT is a
 * measurement, not a preference — the bar records that a SINGLE `"+r"` operand
 * stayed green over a live mutant at debug AND release, because the read half's
 * copy targets a dead vreg and the allocator happened to hand the result the
 * register already holding the right value. Three tied operands remove that
 * luck.
 *
 * ★★ THE VALUES ARE CHOSEN SO A PARTIAL MOVE CANNOT PASS. `kPi`'s bit pattern
 * is 0x400921FB54442D18 — BOTH 32-bit halves significant, so a move that
 * carried only one half produces a number that is not `kPi`. `kE` is
 * 0x402DF854 as a float, all four bytes significant. `kMagic` is 0x51EB851F,
 * likewise. A copy through the wrong register FILE (the pre-fix silent
 * miscompile) returns whatever that unrelated register held, which is
 * essentially never one of these.
 *
 * ★ THE `int` OPERAND IS THE MATCHED DIAGONAL CONTROL. It takes the SAME table
 * through its gpr→gpr cell — the copy that always worked — so a regression that
 * broke the diagonal while adding the off-diagonal reds here and nowhere else.
 *
 * ⚠ THE `volatile` SEEDS ARE LOAD-BEARING: without them the release pipeline
 * folds each operand to a constant before lowering and the optimized arm stops
 * exercising the move it exists to witness.
 *
 * ⚠ THE TEMPLATE IS `nop` ON PURPOSE. The two shipped assembly dialects are
 * different languages (AT&T x86 and aarch64 gas) and share almost no spelling;
 * `nop` is one both declare, so ONE source runs on every leg. The example's
 * subject is the MOVES the expansion emits around the template, not the
 * template's own instruction — an operand that never made the round trip
 * through the integer file comes back wrong whatever sits between the halves.
 */

/* 0x400921FB54442D18 — both 32-bit halves significant. */
static const double kPi = 3.141592653589793;
/* 0x402DF854 — all four bytes significant. */
static const float  kE  = 2.7182817f;
/* 0x51EB851F — all four bytes significant. */
static const int    kMagic = 1374389535;

volatile double gDouble = 3.141592653589793;
volatile float  gFloat  = 2.7182817f;
volatile int    gInt    = 1374389535;

int main(void) {
    int ok = 0;

    /* fpr -> gpr -> fpr at width 64. */
    double d = gDouble;
    __asm__("nop" : "+r"(d));
    if (d == kPi) ok |= 1;

    /* fpr -> gpr -> fpr at width 32 — the OTHER width guard of the same two
     * opcodes (aarch64 FMOV Wd,Sn / FMOV Sd,Wn; x86_64 MOVD). */
    float f = gFloat;
    __asm__("nop" : "+r"(f));
    if (f == kE) ok |= 2;

    /* gpr -> gpr: the DIAGONAL control, through the same table. */
    int n = gInt;
    __asm__("nop" : "+r"(n));
    if (n == kMagic) ok |= 4;

    /* A second 64-bit float operand, so no single arm carries the claim. */
    double d2 = gDouble * 2.0;
    __asm__("nop" : "+r"(d2));
    if (d2 == kPi * 2.0) ok |= 8;

    return ok == 15 ? 42 : 100 + ok;
}
