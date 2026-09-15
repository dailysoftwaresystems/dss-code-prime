/* D-CSUBSET-LONG-DOUBLE-LITERAL-DECODE-PRECISION: an `l`-suffixed long-double
 * literal is decoded at the TARGET's mantissa width, not the host's.
 *
 * ★ WHY THIS EXAMPLE READS BYTES AND NOT A VALUE. The defect was a WRONG NUMBER
 * that was wrong CONSISTENTLY: `0.1L` went through `strtod` into a host
 * `double`, so the literal leaf carried a binary64-rounded value before any
 * fold, and every comparison of one DSS-decoded literal against another agreed
 * with itself. `0.1L == 0.1L` passed; `(int)(10.0L * 0.1L) == 1` passed. Only
 * the BIT PATTERN, held against what a reference compiler actually emits, can
 * see it — so every check below is a byte comparison against a pattern
 * ✔MEASURED from gcc 13.3.0, clang 18.1.3 and MSVC 14.51.36231.
 *
 * The three long-double axes are distinguished from the VALUES THEMSELVES —
 * `sizeof` for the 8-vs-16-byte split, then the exponent field's position in the
 * bytes of `1.0L` (exact on every axis, so the probe is independent of the
 * decode under test). No `#ifdef` on a host or an arch: the axis is a property
 * of the object format, and this file only ever asks what the format did.
 *
 * exit = 42.  Any other code names the check that failed.
 */

/* THE literal: 0.1 needs an infinite binary expansion, so it is inexact in
 * every format and its low bits differ per format. */
static const long double v_tenth = 0.1L;

/* The SAME quantity spelled as a `double` literal and widened. On a binary64
 * axis this must be byte-identical to `v_tenth`; on the x87-80 and ieee128 axes
 * it must DIFFER, because the widen is exact and 0.1 rounded at 53 bits is not
 * 0.1 rounded at 64 or 113. That inequality IS the defect, stated directly:
 * before the fix the two were identical on every axis. */
static const long double v_tenth_via_double = 0.1;

/* The axis probe. 1.0 is exact in binary64, x87-80 and binary128 alike. */
static const long double v_one = 1.0L;

/* Exact midpoints between two adjacent x87-80 normals (1 + 2^-64 and
 * 1 + 2^-63 + 2^-64). Only the round-to-nearest-EVEN rule settles them: the
 * first's lower neighbour has an even significand and it must round DOWN to
 * 1.0; the second's is odd and the same tie must round UP. */
static const long double v_tie_even =
    1.0000000000000000000542101086242752217003726400434970855712890625L;
static const long double v_tie_odd =
    1.0000000000000000001626303258728256651011179201304912567138671875L;

/* The binary128 midpoints (1 + 2^-113 and 1 + 2^-112 + 2^-113). */
/* (One pp-token each — a floating constant may not be split across lines.) */
static const long double v_qtie_even = 1.00000000000000000000000000000000009629649721936179265279889712924636592690508241076940976199693977832794189453125L;
static const long double v_qtie_odd  = 1.00000000000000000000000000000000028888949165808537795839669138773909778071524723230822928599081933498382568359375L;

/* A hex float: already binary, so it must survive exactly at any precision. */
static const long double v_hex = 0x1.fp3L;   /* 15.5 */

static int bytes_are(const long double *v, const unsigned char *want, int n) {
    const unsigned char *b = (const unsigned char *)v;
    int i;
    for (i = 0; i < n; ++i) {
        if (b[i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

static int bytes_differ(const long double *a, const long double *b, int n) {
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    int i;
    for (i = 0; i < n; ++i) {
        if (p[i] != q[i]) {
            return 1;
        }
    }
    return 0;
}

int main(void) {
    const unsigned char *one = (const unsigned char *)&v_one;

    if (sizeof(long double) == 8) {
        /* ── f64 axis (pe64 x86_64 MSVC, macho64 arm64 Apple) ──────────────
         * `long double` IS binary64 here, so a host `double` decode was always
         * the right answer and nothing may move. ✔MEASURED cl.exe 14.51.36231
         * (`/std:c17`, x64): `0.1L` and `0.1` both 9a 99 99 99 99 99 b9 3f, and
         * sizeof(long double) == 8. */
        static const unsigned char w_tenth[8] =
            {0x9a, 0x99, 0x99, 0x99, 0x99, 0x99, 0xb9, 0x3f};
        static const unsigned char w_one[8] =
            {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f};
        if (!bytes_are(&v_tenth, w_tenth, 8))            return 11;
        if (!bytes_are(&v_tenth_via_double, w_tenth, 8)) return 12;
        if (bytes_differ(&v_tenth, &v_tenth_via_double, 8)) return 13;
        /* Both x87-80 midpoints are far inside one binary64 ulp of 1.0, so both
         * round to exactly 1.0 — ✔MEASURED, cl.exe agreeing with gcc/clang. */
        if (!bytes_are(&v_tie_even, w_one, 8))           return 14;
        if (!bytes_are(&v_tie_odd, w_one, 8))            return 15;
        return 42;
    }

    /* A 16-byte slot: x87 80-bit extended, or IEEE binary128. Told apart by
     * where 1.0L keeps its exponent field — bytes 8..9 for x87 extended (whose
     * top 6 bytes are padding), bytes 14..15 for binary128. Neither matching is
     * a representation this example does not know: fail loud, never guess. */
    if (one[8] == 0xff && one[9] == 0x3f) {
        /* ── x87-80 axis (elf64/macho64 x86_64 SysV) ───────────────────────
         * ✔MEASURED gcc 13.3.0 AND clang 18.1.3, probed separately, agreeing
         * byte-for-byte. Only the 10 SIGNIFICANT bytes are compared: the top 6
         * of the 16-byte slot are ABI padding, not part of the value. */
        static const unsigned char w_tenth[10] =
            {0xcd, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xfb, 0x3f};
        static const unsigned char w_widened[10] =   /* what the DEFECT emitted */
            {0x00, 0xd0, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xfb, 0x3f};
        static const unsigned char w_tie_even[10] =
            {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xff, 0x3f};
        static const unsigned char w_tie_odd[10] =
            {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xff, 0x3f};
        static const unsigned char w_hex[10] =
            {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x02, 0x40};
        if (!bytes_are(&v_tenth, w_tenth, 10))              return 21;
        if (bytes_are(&v_tenth, w_widened, 10))             return 22;
        if (!bytes_are(&v_tenth_via_double, w_widened, 10)) return 23;
        if (!bytes_differ(&v_tenth, &v_tenth_via_double, 10)) return 24;
        if (!bytes_are(&v_tie_even, w_tie_even, 10))        return 25;
        if (!bytes_are(&v_tie_odd, w_tie_odd, 10))          return 26;
        if (!bytes_are(&v_hex, w_hex, 10))                  return 27;
        return 42;
    }

    if (one[14] == 0xff && one[15] == 0x3f) {
        /* ── ieee128 axis (elf64 aarch64, AAPCS64) ─────────────────────────
         * ✔MEASURED aarch64-linux-gnu-gcc 13.3.0's own `long double`, agreeing
         * byte-for-byte with x86_64 gcc 13.3.0's `__float128`. */
        static const unsigned char w_tenth[16] =
            {0x9a, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99,
             0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0xfb, 0x3f};
        static const unsigned char w_widened[16] =  /* what the DEFECT emitted */
            {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0,
             0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0xfb, 0x3f};
        static const unsigned char w_qtie_even[16] =
            {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x3f};
        static const unsigned char w_qtie_odd[16] =
            {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x3f};
        static const unsigned char w_hex[16] =
            {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x02, 0x40};
        if (!bytes_are(&v_tenth, w_tenth, 16))              return 31;
        if (bytes_are(&v_tenth, w_widened, 16))             return 32;
        if (!bytes_are(&v_tenth_via_double, w_widened, 16)) return 33;
        if (!bytes_differ(&v_tenth, &v_tenth_via_double, 16)) return 34;
        if (!bytes_are(&v_qtie_even, w_qtie_even, 16))      return 35;
        if (!bytes_are(&v_qtie_odd, w_qtie_odd, 16))        return 36;
        if (!bytes_are(&v_hex, w_hex, 16))                  return 37;
        return 42;
    }

    return 90;   /* a 16-byte long double this example does not recognize */
}
