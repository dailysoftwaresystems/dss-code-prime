// D-PP-IF-UNSIGNED-INTMAX — C 6.10.1p4: a `#if`/`#elif` controlling expression
// is evaluated with every integer type acting as `intmax_t`/`uintmax_t`.
//
// ★★ EVERY CELL IS EXIT-CODE DISCRIMINATING IN BOTH DIRECTIONS, AND THAT SHAPE
// IS THE POINT. This defect selects the WRONG BRANCH and then compiles it
// perfectly — no diagnostic, exit 0, a different program. An example that only
// had to COMPILE would be structurally blind to it, which is exactly how the
// anchor survived four months labelled "fail-loud". So each `#if` writes a
// DIFFERENT value in each arm, and `main` returns 42 only when every cell chose
// the arm gcc and clang choose. Any single wrong branch changes the exit code.
//
// ✔MEASURED 2026-08-27 against gcc 13.3.0 `-std=c2x` and clang 18.1.3
// `-std=c23`, probed SEPARATELY, with the taken arm read out of the EMITTED
// OBJECT via `nm` rather than from an exit code — because an exit code cannot
// see a wrong branch in the compiler that produced it.

// ── The unsigned half: a literal's unsignedness must survive to the fold. ──

// gcc: TRUE. clang: TRUE. DSS before the fix: FALSE (read as a signed -1).
#if 18446744073709551615u > 0
#define C_U64_POSITIVE 1
#else
#define C_U64_POSITIVE 0
#endif

// C 6.3.1.8: `-1` converts to UINTMAX_MAX, so this is FALSE.
// gcc: FALSE. clang: FALSE. DSS before the fix: TRUE (a signed comparison).
#if -1 < 0u
#define C_MIXED_IS_UNSIGNED 0
#else
#define C_MIXED_IS_UNSIGNED 1
#endif

// Unsigned division, remainder and a LOGICAL right shift.
#if (18446744073709551615u / 2u) == 9223372036854775807
#define C_U64_DIV 1
#else
#define C_U64_DIV 0
#endif
#if (18446744073709551615u % 10u) == 5
#define C_U64_MOD 1
#else
#define C_U64_MOD 0
#endif
#if (18446744073709551615u >> 60) == 15
#define C_U64_SHR 1
#else
#define C_U64_SHR 0
#endif
#if 0u - 1u > 0
#define C_U64_WRAP 1
#else
#define C_U64_WRAP 0
#endif

// ── The WIDTH half, which needs no unsigned literal at all. ──
// The leaf used to stamp every literal `int`, so the whole evaluator ran in
// 32-BIT signed — not the "signed int64" the anchor claimed — and both operands
// were truncated before every comparison. "Is INT64_MAX positive" answered NO.

#if 9223372036854775807 > 0
#define C_INTMAX_POSITIVE 1
#else
#define C_INTMAX_POSITIVE 0
#endif
#if 3000000000 > 0
#define C_ABOVE_INT32 1
#else
#define C_ABOVE_INT32 0
#endif
#if 2147483647 + 1 > 0
#define C_NO_32BIT_OVERFLOW 1
#else
#define C_NO_32BIT_OVERFLOW 0
#endif
#if (1 << 40) > 0
#define C_SHIFT_PAST_32 1
#else
#define C_SHIFT_PAST_32 0
#endif

// ── The signedness BOUNDARY is INTMAX_MAX, not the literal's own C type. ──
// C 6.4.4.1 types `0xFFFFFFFF` as `unsigned int`; phase 4 re-decides at 64 bits
// and both references treat it as SIGNED. The adjacent pair below straddles
// INTMAX_MAX exactly, so the boundary is pinned rather than asserted.

#if 0x7FFFFFFFFFFFFFFF > -1
#define C_AT_INTMAX_MAX_IS_SIGNED 1
#else
#define C_AT_INTMAX_MAX_IS_SIGNED 0
#endif
#if 0x8000000000000000 > -1
#define C_ABOVE_INTMAX_MAX_IS_UNSIGNED 0
#else
#define C_ABOVE_INTMAX_MAX_IS_UNSIGNED 1
#endif
#if 0xFFFFFFFF > -1
#define C_HEX32_IS_SIGNED_IN_PHASE4 1
#else
#define C_HEX32_IS_SIGNED_IN_PHASE4 0
#endif

// A SUFFIX still forces unsignedness at any magnitude — the control against an
// implementation that decided by magnitude alone and ignored suffixes.
#if 0xFFFFFFFFu > -1
#define C_SUFFIX_STILL_FORCES_UNSIGNED 0
#else
#define C_SUFFIX_STILL_FORCES_UNSIGNED 1
#endif

// A decimal literal above INTMAX_MAX whose ladder is entirely SIGNED.
// ✔MEASURED: gcc warns "integer constant is so large that it is unsigned",
// clang warns "interpreting as unsigned", and BOTH then take the TRUE arm.
#if 18446744073709551615 > 0
#define C_DECIMAL_ABOVE_INTMAX 1
#else
#define C_DECIMAL_ABOVE_INTMAX 0
#endif

// A controlling expression whose RESULT is a legitimate unsigned value above
// INT64_MAX. Fixing only the literal leaf turns this into a spurious REFUSAL,
// because the old closing bridge asked whether the result fit an int64 — a
// question C 6.10.1p2 never asks. Both halves land together or this arm flips.
#if 18446744073709551615u
#define C_LARGE_UNSIGNED_IS_TRUTHY 1
#else
#define C_LARGE_UNSIGNED_IS_TRUTHY 0
#endif

// ── The REGRESSION direction: widening must not make signed behave unsigned. ──
#if -1 < 0
#define C_SIGNED_STILL_SIGNED 1
#else
#define C_SIGNED_STILL_SIGNED 0
#endif
#if (-1 >> 1) == -1
#define C_ARITHMETIC_SHR 1
#else
#define C_ARITHMETIC_SHR 0
#endif
#if (-1 / 2) == 0
#define C_SIGNED_DIV 1
#else
#define C_SIGNED_DIV 0
#endif

// THE CONTROL: all three implementations agree, so this example can demonstrate
// AGREEMENT and is not merely a collection of divergences.
#if 2 + 2 == 4
#define C_CONTROL 1
#else
#define C_CONTROL 0
#endif

// `#elif` runs the SAME evaluator, so the fix must reach it too. The first arm
// is false under correct intmax rules and TRUE under the old 32-bit signed
// reading, so a regression here picks arm 1 instead of arm 2.
#if 18446744073709551615u < 0
#define C_ELIF_ARM 1
#elif 18446744073709551615u > 0
#define C_ELIF_ARM 2
#else
#define C_ELIF_ARM 3
#endif

int main(void) {
    // Each cell contributes exactly 1. Anything that took the other arm
    // contributes 0 and the exit code moves — no cell can be masked by another.
    int ok = 0;
    ok += C_U64_POSITIVE;                    /*  1 */
    ok += C_MIXED_IS_UNSIGNED;               /*  2 */
    ok += C_U64_DIV;                         /*  3 */
    ok += C_U64_MOD;                         /*  4 */
    ok += C_U64_SHR;                         /*  5 */
    ok += C_U64_WRAP;                        /*  6 */
    ok += C_INTMAX_POSITIVE;                 /*  7 */
    ok += C_ABOVE_INT32;                     /*  8 */
    ok += C_NO_32BIT_OVERFLOW;               /*  9 */
    ok += C_SHIFT_PAST_32;                   /* 10 */
    ok += C_AT_INTMAX_MAX_IS_SIGNED;         /* 11 */
    ok += C_ABOVE_INTMAX_MAX_IS_UNSIGNED;    /* 12 */
    ok += C_HEX32_IS_SIGNED_IN_PHASE4;       /* 13 */
    ok += C_SUFFIX_STILL_FORCES_UNSIGNED;    /* 14 */
    ok += C_DECIMAL_ABOVE_INTMAX;            /* 15 */
    ok += C_LARGE_UNSIGNED_IS_TRUTHY;        /* 16 */
    ok += C_SIGNED_STILL_SIGNED;             /* 17 */
    ok += C_ARITHMETIC_SHR;                  /* 18 */
    ok += C_SIGNED_DIV;                      /* 19 */
    ok += C_CONTROL;                         /* 20 */

    // The `#elif` chain contributes its ARM NUMBER, so picking arm 1 or arm 3
    // is distinguishable from picking arm 2 rather than merely "not 2".
    ok += C_ELIF_ARM;                        /* +2 => 22 */

    // 22 correct cells. Scaled and offset so a single wrong branch cannot land
    // on 42 by arithmetic accident: 22*2 - 2 == 42.
    return ok * 2 - 2;
}
