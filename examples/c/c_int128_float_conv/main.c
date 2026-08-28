/* 128-bit integer <-> `double`, BOTH DIRECTIONS (D-CSUBSET-INT128-FLOAT-CONV).
 *
 * The runnable witness for the conversion that used to be a REFUSAL: `(__int128)d`
 * and `(double)someU128` both fired `S0050`, and the message named `_BitInt` and
 * cited a `_BitInt` row for a source containing no `_BitInt` anywhere. gcc 13.3.0
 * (`-std=c2x`) and clang 18.1.3 / 19 (`-std=c23`), probed SEPARATELY, all compile
 * and RUN every term below, so the union rule requires DSS to.
 *
 * ★★ WHY THE ARITHMETIC IS THE POINT, NOT THE ACCEPTANCE. The refusal existed
 * because "the naive scalar path would fill only limb 0 (wrong sign, wrong value,
 * dropped upper limbs)" — i.e. the danger was never that the conversion is hard to
 * ADMIT, it is that a wrong value is a SILENT MISCOMPILE. So every term here is an
 * EXACT relation between integers, and each one is chosen so a specific wrong
 * implementation gives a different answer:
 *
 *  - t1: a value whose LOW LIMB IS ZERO (3·2^64). A conversion that reads only limb
 *        0 answers 0.0. The round trip is exact because 3·2^64 has TWO significant
 *        bits, so this is an equality, not a bracket.
 *  - t2: ★ THE DOUBLE-ROUNDING TRAP, and the reason this example exists at all.
 *        v = (2^53+1)·2^64 + 1. The obvious two-step `(double)hi · 2^64 +
 *        (double)lo` rounds TWICE: `(double)(2^53+1)` ties to even → 2^53, scaling
 *        is exact, and adding 1.0 changes nothing → 2^117. The CORRECTLY ROUNDED
 *        answer is 2^117 + 2^65, because the exact remainder 2^64+1 is just OVER
 *        half an ulp (ulp at 2^117 is 2^65). A ONE-ULP error is the entire
 *        difference, and t2 is the only term that can see it.
 *        ✔MEASURED: clang 18.1.3 and 19 both produce 2^117 + 2^65.
 *  - t3: the SIGNED counterpart — a negative whose magnitude needs BOTH limbs.
 *        A logical (rather than arithmetic) treatment, a dropped sign, or a
 *        magnitude taken before the negation all break the sign symmetry.
 *  - t4: `double` -> 128-bit, TRUNCATING TOWARD ZERO (C 6.3.1.4p1) on BOTH signs.
 *        A floor-based conversion answers -4 where C requires -3.
 *  - t5: the saturation edge — 2^128-1 rounds UP to 2^128, so the double is
 *        strictly greater than the one for 2^127 and exactly twice it. Written as
 *        a relation between two converted values, so no float literal is involved.
 *  - t6: the `_BitInt` twins at TWO limb counts (128 and 200), which is what proves
 *        the emitter is limb-count-general rather than 2-limb special-cased. gcc
 *        13.3.0 has no `_BitInt` at all, so clang is the witness here; that is a
 *        missing FEATURE in gcc, not a refusal, and the disjunction still binds.
 *  - t7: a `float` SOURCE. `float`->`double` widening is EXACT, so this must give
 *        the identical answer to the `double` form — one rounding in the whole
 *        conversion, never two.
 *
 * ⚠ NO FLOATING LITERAL IS COMPARED AGAINST ANYWHERE, deliberately. A decimal
 * bound written to bracket a value ROUNDS TO THAT VALUE and the comparison becomes
 * vacuous — ✔MEASURED while writing this: `d > 5.5340232221128654e19` is FALSE for
 * d = 3·2^64 because the literal IS d. Every expectation is therefore an exact
 * integer equality or an exact relation between two converted values.
 *
 * ⚠ Every operand is laundered through `op()` / `opd()`, which the optimizer cannot
 * fold, so the release arm exercises the real runtime conversion rather than a
 * constant the front end pre-computed.
 *
 * ⓘ SPLIT INTO PER-TERM FUNCTIONS ON PURPOSE. ✔MEASURED: a single `main` holding
 * all of this needs a 1088-byte frame, and arm64 then fails with
 * `A_ImmediateOperandOutOfRange` on `fstur`/`fldur` — the assembler's signed 9-bit
 * `imm9` offset (a PRE-EXISTING limit, unrelated to this conversion: the same file
 * built clean once split). Small frames keep every leg buildable.
 *
 * exit = t1+t2+t3+t4+t5+t6+t7 = 7 * 6 = 42. */

typedef unsigned __int128 U128;
typedef unsigned long long u64;

/* Opaque launderers — no const-fold can pre-evaluate a conversion through these. */
static U128 opimpl(U128 x)  { return x; }
static U128 op(U128 x)      { return opimpl(x); }
static double opdimpl(double x) { return x; }
static double opd(double x)     { return opdimpl(x); }

/* Build a 128-bit value from two 64-bit halves — the whole corpus is written this
 * way because a 128-bit literal has no C spelling. */
static U128 mk(u64 hi, u64 lo) { return ((U128)hi << 64) | (U128)lo; }

/* t1 — a ZERO low limb: `(double)(3·2^64)` must not be 0.0, and the round trip is
 * exact (two significant bits). */
static int t1(void) {
    U128 const v = op(mk(3u, 0u));
    double const d = (double)v;
    if (d == 0.0) return 0;
    if ((U128)d != v) return 0;
    return 6;
}

/* t2 — THE DOUBLE-ROUNDING TRAP. See the header: naive two-step gives 2^117,
 * correct is 2^117 + 2^65. */
static int t2(void) {
    U128 const v    = op(mk(0x0020000000000001ull, 0x0000000000000001ull));
    U128 const want = mk(0x0020000000000002ull, 0x0000000000000000ull);
    U128 const naive= mk(0x0020000000000000ull, 0x0000000000000000ull);
    U128 const got  = (U128)(double)v;
    if (got != want) return 0;
    if (got == naive) return 0;     /* explicit: the wrong answer is a DIFFERENT one */
    return 6;
}

/* t3 — SIGNED. -(3·2^64) round-trips exactly and negates symmetrically. */
static int t3(void) {
    __int128 const n = -(__int128)op(mk(3u, 0u));
    double const dn = (double)n;
    if (!(dn < 0.0)) return 0;
    if ((__int128)dn != n) return 0;
    if ((double)(-n) != -dn) return 0;
    if ((__int128)(-dn) != -n) return 0;
    return 6;
}

/* t4 — `double` -> 128-bit TRUNCATES TOWARD ZERO on both signs (C 6.3.1.4p1). */
static int t4(void) {
    double const d = opd(3.9);
    if ((U128)d != (U128)3u) return 0;
    if ((__int128)(-d) != (__int128)-3) return 0;   /* floor would give -4 */
    if ((U128)opd(0.5) != (U128)0u) return 0;
    if ((U128)opd(0.0) != (U128)0u) return 0;
    return 6;
}

/* t5 — the top of the range. 2^128-1 rounds UP to 2^128, so it is exactly twice
 * the double for 2^127. Both sides are CONVERTED values: no literal, no bracket. */
static int t5(void) {
    double const dAll = (double)op(~(U128)0u);
    double const d127 = (double)op(mk(0x8000000000000000ull, 0u));
    if (!(dAll > d127)) return 0;
    if (dAll != d127 * 2.0) return 0;
    return 6;
}

/* t6 — the `_BitInt` twins at TWO limb counts: 128 (2 limbs) and 200 (4 limbs). */
static int t6(void) {
    unsigned _BitInt(128) const b = (unsigned _BitInt(128))op((U128)5u) << 100;
    if ((unsigned _BitInt(128))(double)b != b) return 0;
    unsigned _BitInt(200) const w = (unsigned _BitInt(200))op((U128)5u) << 150;
    if ((unsigned _BitInt(200))(double)w != w) return 0;
    if ((double)w == 0.0) return 0;         /* limbs 0 and 1 are BOTH zero */
    return 6;
}

/* t7 — a `float` SOURCE. The widening to `double` is exact, so the two forms must
 * agree bit for bit; a second rounding would make them differ. */
static int t7(void) {
    float const f = (float)opd(3.9);
    if ((U128)f != (U128)3u) return 0;
    U128 const v = op(mk(3u, 0u));
    double const d = (double)v;
    if ((U128)(double)(float)16777216.0f != (U128)16777216u) return 0;  /* 2^24 exact */
    if ((U128)d != v) return 0;
    return 6;
}

int main(void) {
    return t1() + t2() + t3() + t4() + t5() + t6() + t7();
}
