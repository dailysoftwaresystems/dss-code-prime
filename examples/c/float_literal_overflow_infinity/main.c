/* D-C-FLOAT-LITERAL-OVERFLOW-REFUSED-INSTEAD-OF-YIELDING-INFINITY: a floating
 * literal too large for its type compiles, and carries the infinity the
 * references emit.
 *
 * ★ WHY THIS PROGRAM EXISTS. Every literal below except the controls was a HARD
 * COMPILE ERROR before this fix — `error[H_UnsupportedLoweringForKind]: literal
 * '1e400' is out of range / undecodable` (✔MEASURED 2026-09-02 at
 * x86_64:pe64-x86_64-windows-exec). ISO C never asked for that refusal:
 * C23 5.2.5.3.3¶19 and Annex F.2.2¶1 extend a type's representable RANGE to
 * every real number once its infinities are representable, so on an IEC 60559
 * implementation the correctly-rounded value of `1e400` IS +∞ — the nearest
 * representable value, exactly as 0x3FB999999999999A is 0.1's.
 *
 * ★★ THE REFERENCES SPLIT 3–1 ON ACCEPTANCE, ✔MEASURED 2026-09-02 with each one
 * invoked SEPARATELY: gcc 13.3.0, clang 18.1.3 (both WSL) and mingw-w64 gcc
 * 13.2.0 accept every literal here — warning `-Woverflow` / `-Wliteral-range` —
 * and all three emit the SAME bytes; MSVC 19.51.36252 refuses each one with
 * `error C2177: constant too big`. An accept-vs-refuse split is what the
 * disjunction governs, and one working reference makes the behaviour REQUIRED.
 *
 * ★★ AND IT READS BYTES, NOT "it compiles". A compile-only pin cannot tell +∞
 * from a saturation to DBL_MAX, or from a value silently truncated on the way
 * to the pool — which is the exact failure mode a fix in this direction risks.
 * Every pattern below was ✔MEASURED from the static initializer a reference
 * compiler ACTUALLY EMITTED.
 *
 * ⚠ THE BOUNDARY IS PINNED FROM BOTH SIDES, one ulp apart, because that pair is
 * the only check separating "accepts overflow" from "stopped checking range".
 * DBL_MAX must still be DBL_MAX; the next decimal above it must be +∞.
 *
 * ⓘ The `long double` half needs an AXIS PROBE and the `double`/`float` half
 * does not: `double` is binary64 and `float` binary32 on every target DSS ships,
 * while `long double` is binary64 on two legs, x87-80 on one and binary128 on
 * one. The axis is read from the VALUES THEMSELVES — `sizeof`, then where
 * `1.0L` keeps its exponent field — never from a host or arch `#ifdef`, exactly
 * as `c_long_double_literal_decode` does it.
 *
 * exit = 42.  Any other code names the check that failed.
 */

/* ── binary64: the row's own literal, and the shapes that reach the same door ── */

/* The literal the row was filed on. */
static const double d_over = 1e400;

/* The sign survives. `-` is a unary operator here, not part of the constant, so
 * this also pins that the fold negates an infinity rather than refusing it. */
static const double d_neg_over = -1e400;

/* Barely over: the first power of ten past DBL_MAX. */
static const double d_309 = 1e309;

/* ★ THE BOUNDARY. One ulp past DBL_MAX — the smallest decimal here that is NOT
 * DBL_MAX itself. gcc/clang/mingw-gcc emit +∞; MSVC refuses exactly here. */
static const double d_just_over = 1.7976931348623159e308;

/* A HEX float overflows through the same strtod door as a decimal one. */
static const double d_hex_over     = 0x1p+99999;
static const double d_neg_hex_over = -0x1p+99999;

/* ⚠ THE CONTROLS, and the direction the risk runs. The defect refused a correct
 * program; an over-correction accepts a value the source never named. DBL_MAX is
 * finite and must stay its own bits. */
static const double d_max      = 1.7976931348623157e308;
static const double d_ordinary = 0.1;

/* ── binary32 ─────────────────────────────────────────────────────────────────
 * ★ `f_over` is the one literal in this file that ALREADY COMPILED before the
 * fix, and it is here because of what that means. ✔MEASURED 2026-09-02: DSS
 * accepted `1e40f` and emitted `7f800000` — a silent, correct +∞ — because 1e40
 * is an ordinary `double` and the overflow happens later, in the F32 narrowing.
 * So DSS already produced the reference bytes for a `float` literal overflowing
 * `float` while refusing the identical question one type up. The fix removed an
 * inconsistency; this line is the proof the F32 side did not move. */
static const float f_over     = 1e40f;
static const float f_neg_over = -1e40f;

/* `1e400f` and `0x1p+9999f`, by contrast, overflow `double` on the way and WERE
 * refused — the same door as `d_over`, reached through a `float`-typed literal. */
static const float f_double_over = 1e400f;
static const float f_hex_over    = 0x1p+9999f;

static const float f_max      = 3.40282347e38f;
static const float f_ordinary = 0.5f;

/* ── long double: overflows every axis ────────────────────────────────────────
 * 1e5000 is past binary64's ~1.8e308 AND past the ~1.19e4932 that x87-80 and
 * binary128 share (both carry a 15-bit exponent), so ONE literal overflows all
 * three axes and only the expected PATTERN differs. */
static const long double ld_over = 1e5000L;

/* The axis probe. 1.0 is exact in binary64, x87-80 and binary128 alike, so this
 * value is independent of the decode under test. */
static const long double ld_one = 1.0L;

static int bytes_are(const void *v, const unsigned char *want, int n) {
    const unsigned char *b = (const unsigned char *)v;
    int i;
    for (i = 0; i < n; ++i) {
        if (b[i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    /* 0x7FF0000000000000 / 0xFFF0000000000000 — ✔MEASURED, gcc 13.3.0,
     * clang 18.1.3 and mingw-w64 gcc 13.2.0 all emitting the same bytes. */
    static const unsigned char w_dinf[8] =
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x7f};
    static const unsigned char w_dninf[8] =
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0xff};
    /* 0x7FEFFFFFFFFFFFFF — DBL_MAX, which all FOUR references accept. */
    static const unsigned char w_dmax[8] =
        {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xef, 0x7f};
    /* 0x3FB999999999999A */
    static const unsigned char w_tenth[8] =
        {0x9a, 0x99, 0x99, 0x99, 0x99, 0x99, 0xb9, 0x3f};
    /* 0x7F800000 / 0xFF800000 / 0x7F7FFFFF / 0x3F000000 */
    static const unsigned char w_finf[4]  = {0x00, 0x00, 0x80, 0x7f};
    static const unsigned char w_fninf[4] = {0x00, 0x00, 0x80, 0xff};
    static const unsigned char w_fmax[4]  = {0xff, 0xff, 0x7f, 0x7f};
    static const unsigned char w_fhalf[4] = {0x00, 0x00, 0x00, 0x3f};

    const unsigned char *one = (const unsigned char *)&ld_one;

    if (!bytes_are(&d_over, w_dinf, 8))          return 11;
    if (!bytes_are(&d_neg_over, w_dninf, 8))     return 12;
    if (!bytes_are(&d_309, w_dinf, 8))           return 13;
    if (!bytes_are(&d_just_over, w_dinf, 8))     return 14;
    if (!bytes_are(&d_hex_over, w_dinf, 8))      return 15;
    if (!bytes_are(&d_neg_hex_over, w_dninf, 8)) return 16;
    if (!bytes_are(&d_max, w_dmax, 8))           return 17;
    if (!bytes_are(&d_ordinary, w_tenth, 8))     return 18;

    if (!bytes_are(&f_over, w_finf, 4))          return 21;
    if (!bytes_are(&f_neg_over, w_fninf, 4))     return 22;
    if (!bytes_are(&f_double_over, w_finf, 4))   return 23;
    if (!bytes_are(&f_hex_over, w_finf, 4))      return 24;
    if (!bytes_are(&f_max, w_fmax, 4))           return 25;
    if (!bytes_are(&f_ordinary, w_fhalf, 4))     return 26;

    if (sizeof(long double) == 8) {
        /* ── f64 axis (pe64 x86_64 MSVC ABI, macho64 arm64 Apple) ──────────
         * `long double` IS binary64 here, so `1e5000L` overflows exactly as
         * `1e400` does and takes the very same decoder door. */
        if (!bytes_are(&ld_over, w_dinf, 8)) return 31;
        return 42;
    }

    /* A 16-byte slot: x87 80-bit extended, or IEEE binary128. Told apart by
     * where 1.0L keeps its exponent field — bytes 8..9 for x87 extended (whose
     * top 6 bytes are ABI padding), bytes 14..15 for binary128. Neither matching
     * is a representation this example does not know: fail loud, never guess. */
    if (one[8] == 0xff && one[9] == 0x3f) {
        /* ── x87-80 axis (elf64/macho64 x86_64 SysV) ───────────────────────
         * ✔MEASURED gcc 13.3.0 AND clang 18.1.3, probed separately, agreeing
         * byte-for-byte on `1e5000L`. Only the 10 SIGNIFICANT bytes are
         * compared: the top 6 of the 16-byte slot are padding, not value. */
        static const unsigned char w_x87inf[10] =
            {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xff, 0x7f};
        if (!bytes_are(&ld_over, w_x87inf, 10)) return 41;
        return 42;
    }

    if (one[14] == 0xff && one[15] == 0x3f) {
        /* ── ieee128 axis (elf64 aarch64, AAPCS64) ─────────────────────────
         * ✔MEASURED from gcc 13.3.0's and clang 18.1.3's `__float128` on
         * x86_64, which is the same binary128 aarch64 `long double` uses. */
        static const unsigned char w_q128inf[16] =
            {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x7f};
        if (!bytes_are(&ld_over, w_q128inf, 16)) return 51;
        return 42;
    }

    return 90;   /* a 16-byte long double this example does not recognize */
}
