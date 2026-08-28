/* D-TARGET-ENCODING-TABLE-EXPRESSES-ONLY-THE-DEGENERATE-SEQUENCE
 * (closes D-CSUBSET-UI-TO-FP-UNSIGNED-I64 + D-CSUBSET-UI-FROM-FP-UNSIGNED-I64).
 *
 * The FULL unsigned range of both integer<->floating-point directions, at BOTH
 * source float widths. Until 2026-08-24 the x86_64 opcode table declared the
 * SIGNED converters (CVTTSD2SI / CVTSI2SD) as if they were the unsigned ones,
 * because the table could only say "this opcode IS one instruction" and x86-64
 * SSE2 has no unsigned instruction to name. FOUR arms miscompiled SILENTLY —
 * no diagnostic, on every x86_64 format, in debug AND release:
 *
 *   1  (unsigned long long)1.0e19   gave 2^63          (F64 -> u64 at/above 2^63)
 *   2  (unsigned long long)1.0e19f  gave 2^63          (F32 -> u64, in NO registry row)
 *   3  (double)(1ULL << 63)         gave a NEGATIVE    (u64 -> F64 at/above 2^63)
 *   4  (double)(unsigned)0xFFFFFFFF gave -1.0          (u32 -> F64 at/above 2^31,
 *                                                       in NO registry row either)
 *
 * gcc-13 and clang-19 agree with each other on every value below; arm64 was
 * already right on all of them (UCVTF / FCVTZU are natively full-range), which
 * is why this witness runs on BOTH targets: the arm64 legs prove the ONE-STEP
 * declared sequence is genuinely read and emitted, not merely tolerated.
 *
 * Every value crosses a `volatile` sink before it is converted, so the
 * CONVERSION happens at run time on the target — the only place the defect
 * ever existed. (`io()` supplies the integer side; there is deliberately no
 * int->float cast anywhere, which is still a fail-loud refusal on both targets
 * under D-CSUBSET-INT-TO-F32-CODEGEN.)
 *
 * RED-ON-DISABLE:
 *   x86  — restore either `ui_to_fp`/`fp_to_ui` single-instruction `encoding`
 *          block in x86_64.target.json in place of its `lowering` sequence:
 *          arms 1-4 come back and this exits 51/52/53/54 instead of 42.
 *   arm64 — point either arm64 `lowering` step at the SIGNED sibling
 *          (`si_to_fp` / `fp_to_si`): the arm64 legs reproduce the x86 defect.
 *   substrate — make `lowerViaDeclaredSequence` return false: x86 has no
 *          encoding left to fall back to, so the build fails
 *          A_NoEncodingDeclared rather than silently miscompiling.
 * => 42.
 */

int io(int x) { return x; }

volatile double             gd;
volatile float              gf;
volatile unsigned long long gu64;
volatile unsigned int       gu32;
volatile long long          gi64;

int main(void) {
    /* ---- fp_to_ui, F64 source, at and above 2^63 ---- */
    gd = 1.0e19;
    gu64 = (unsigned long long)gd;
    if (gu64 != 10000000000000000000ULL) return 51;

    gd = 9223372036854775808.0;                  /* exactly 2^63 */
    gu64 = (unsigned long long)gd;
    if (gu64 != 9223372036854775808ULL) return 51;

    /* ---- fp_to_ui, F32 source (the arm no registry row named) ---- */
    gf = 1.0e19f;
    gu64 = (unsigned long long)gf;
    if (gu64 != 9999999980506447872ULL) return 52;

    /* ---- ui_to_fp, u64 source, at and above 2^63 ---- */
    gu64 = (unsigned long long)io(1) << 63;
    gd = (double)gu64;
    if (gd != 9223372036854775808.0) return 53;

    gu64 = 0xFFFFFFFFFFFFFFFFULL;
    gd = (double)gu64;
    if (gd != 18446744073709551616.0) return 53;

    /* ---- ui_to_fp, u32 source, at and above 2^31 (the other unnamed arm) ---- */
    gu32 = 0xFFFFFFFFu;
    gd = (double)gu32;
    if (gd != 4294967295.0) return 54;

    gu32 = 0x80000000u;
    gd = (double)gu32;
    if (gd != 2147483648.0) return 54;

    /* ---- the IN-RANGE controls: the values that were always correct must
     *      still be correct. A "fix" that only moved the wrongness would
     *      pass the six checks above and fail here. ---- */
    gd = 123.5;
    gu64 = (unsigned long long)gd;
    if (gu64 != 123ULL) return 55;

    gf = 123.5f;
    gu64 = (unsigned long long)gf;
    if (gu64 != 123ULL) return 55;

    gu64 = (unsigned long long)io(123);
    gd = (double)gu64;
    if (gd != 123.0) return 56;

    gd = 4.0e9;
    gu32 = (unsigned int)gd;
    if (gu32 != 4000000000u) return 57;

    gu32 = (unsigned int)io(1000000);
    gd = (double)gu32;
    if (gd != 1000000.0) return 58;

    /* ---- and the SIGNED conversions must be untouched: the x86 unsigned
     *      sequences are built OUT OF si_to_fp / fp_to_si, so a wrong wiring
     *      would show up here as well as above. ---- */
    gi64 = -(((long long)io(1)) << 40);          /* -2^40 */
    gd = (double)gi64;
    if (gd != -1099511627776.0) return 59;

    gd = -123.5;
    gi64 = (long long)gd;
    if (gi64 != -123LL) return 60;

    return 42;
}
