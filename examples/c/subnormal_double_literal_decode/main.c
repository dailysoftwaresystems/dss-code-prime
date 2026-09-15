/* D-C-DECODEFLOAT-TREATS-UNDERFLOW-AS-FATAL: a `double` literal that
 * UNDERFLOWS compiles, and carries the reference bit pattern.
 *
 * ★ WHY THIS PROGRAM EXISTS AT ALL. Every literal below was a HARD COMPILE
 * ERROR before this fix — `error[H_UnsupportedLoweringForKind]: literal
 * '1e-320' is out of range / undecodable` (✔MEASURED 2026-09-02 at
 * x86_64:pe64-x86_64-windows-exec). `decodeFloat` read `strtod`'s `ERANGE` as a
 * single verdict, and C gives that one errno to two OPPOSITE outcomes: an
 * OVERFLOW returns ±HUGE_VAL, an UNDERFLOW returns a magnitude no greater than
 * the smallest normal. Underflow to a subnormal is an ordinary representable
 * binary64 value, so refusing it refused correct programs. That this file
 * COMPILES is already half the witness; the byte checks are the other half.
 *
 * ★★ AND WHY IT READS BYTES. "It compiles now" cannot tell a correct decode
 * from a decode that silently lost the value — the exact failure mode a
 * compile-only pin is blind to. Every pattern below was ✔MEASURED 2026-09-02
 * from the static initializer a reference compiler ACTUALLY EMITTED, with all
 * four agreeing byte-for-byte: gcc 13.3.0 and clang 18.1.3 (WSL, probed
 * separately), mingw-w64 gcc 13.2.0, and MSVC cl.exe 19.51.36252 (which accepts
 * every one of them silently, no warning even at /W4).
 *
 * ⓘ NO AXIS PROBE HERE, unlike its `long double` sibling. `double` is binary64
 * on every target DSS ships, so ONE expected pattern is right on all four legs
 * — there is nothing for a per-format branch to decide.
 *
 * exit = 42.  Any other code names the check that failed.
 */

/* The smallest NORMAL binary64. Its neighbour one ulp below is the largest
 * subnormal, so this pair straddles the boundary the old predicate drew. This
 * one always compiled — it sets no ERANGE — and pins that nothing moved on the
 * side of the line that was already correct. */
static const double v_smallest_normal = 2.2250738585072014e-308;

/* The largest SUBNORMAL: 0x000FFFFFFFFFFFFF, one ulp below the value above. */
static const double v_largest_subnormal = 2.225073858507201e-308;

/* A mid-range subnormal — the literal the row was filed on. */
static const double v_mid_subnormal = 1e-320;

/* The smallest subnormal, 0x1 — one single significand bit left. */
static const double v_smallest_subnormal = 5e-324;

/* Just above the halfway point to that smallest subnormal, so round-to-nearest
 * carries it UP to 0x1 rather than down to zero. */
static const double v_rounds_up_to_min = 3e-324;

/* ★ THE FLUSH-TO-ZERO BOUNDARY, the over-correction's trap. Both of these
 * underflow all the way to ZERO and are STILL accepted by all four references
 * (gcc/clang/mingw warn; MSVC is silent) — so "refuse everything that
 * underflows" and "accept nothing that reaches zero" are both wrong. `2e-324`
 * is just BELOW the halfway point to the smallest subnormal, `1e-330` is far
 * below it; both are exactly +0.0. */
static const double v_flush_to_zero = 2e-324;
static const double v_far_below     = 1e-330;

/* An ordinary literal, as the CONTROL: it never went near ERANGE and its bytes
 * must be untouched by any of this. */
static const double v_ordinary = 0.1;

static int bytes_are(const double *v, const unsigned char *want) {
    const unsigned char *b = (const unsigned char *)v;
    int i;
    for (i = 0; i < 8; ++i) {
        if (b[i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    /* 0x0010000000000000 */
    static const unsigned char w_smallest_normal[8] =
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00};
    /* 0x000FFFFFFFFFFFFF */
    static const unsigned char w_largest_subnormal[8] =
        {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00};
    /* 0x00000000000007E8 */
    static const unsigned char w_mid_subnormal[8] =
        {0xe8, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    /* 0x0000000000000001 */
    static const unsigned char w_one_ulp[8] =
        {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    /* 0x0000000000000000 */
    static const unsigned char w_zero[8] =
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    /* 0x3FB999999999999A */
    static const unsigned char w_tenth[8] =
        {0x9a, 0x99, 0x99, 0x99, 0x99, 0x99, 0xb9, 0x3f};

    if (!bytes_are(&v_smallest_normal, w_smallest_normal))     return 11;
    if (!bytes_are(&v_largest_subnormal, w_largest_subnormal)) return 12;
    if (!bytes_are(&v_mid_subnormal, w_mid_subnormal))         return 13;
    if (!bytes_are(&v_smallest_subnormal, w_one_ulp))          return 14;
    if (!bytes_are(&v_rounds_up_to_min, w_one_ulp))            return 15;
    if (!bytes_are(&v_flush_to_zero, w_zero))                  return 16;
    if (!bytes_are(&v_far_below, w_zero))                      return 17;
    if (!bytes_are(&v_ordinary, w_tenth))                      return 18;

    /* ⚠ DELIBERATELY NO ARITHMETIC OR COMPARISON ON THESE VALUES. The obvious
     * extra guard — `v_smallest_subnormal > v_flush_to_zero`, or doubling a
     * subnormal — would be a claim about the RUNTIME FPU, not about the
     * decode: a leg running with flush-to-zero enabled reads both operands as
     * zero and the check goes red over a perfectly correct literal. This lane
     * can execute only one of the four legs, so importing a hardware-mode
     * dependency here would be a portability claim measured on one host. The
     * byte checks above are immune (they read stored memory) and already catch
     * everything the comparisons would: a decode that collapsed underflow to
     * zero cannot match `e8 07 00 …` or `01 00 00 …`. */

    return 42;
}
