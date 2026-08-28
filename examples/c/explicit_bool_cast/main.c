/* `(_Bool)x` — THE EXPLICIT CAST, which is the one route to `_Bool` that did not
 * use the truthiness chokepoint (D-CSUBSET-EXPLICIT-BOOL-CAST-IS-A-TRUNCATION).
 *
 * C 6.3.1.2 defines conversion of ANY scalar to `_Bool` as "compares equal to 0":
 * 0 becomes false and ANY nonzero value becomes true. The DECLARATION family
 * (`_Bool b = x;`, a call argument, a return) already realized that through
 * `coerceCondition`, the one verb that owns the truth-value vocabulary. The
 * EXPLICIT cast did not: it fell through to a plain `Cast` node, and HIR→MIR's
 * `mapCast` classifies `_Bool` as an 8-bit integer, so the pair became a WIDTH
 * conversion — `Trunc`, which keeps only the LOW BIT.
 *
 * ✔MEASURED at 301e2a63, `dsscp --compile`, on x86_64:pe64-x86_64-windows-exec AND
 * x86_64:elf64-x86_64-linux-exec: `_Bool b = (_Bool)x;` on an `int` walls with
 *     error[L_UnsupportedLoweringForOpcode] Trunc result: TypeKind ordinal 0 has no
 *     encoded conversion form on target 'x86_64' this cycle
 * — so LIR's missing `_Bool`-width `Trunc` form was the ONLY thing standing between
 * this and a silent miscompile, because the instruction MIR asked for computes
 * `(_Bool)256 == 0` where C requires 1. gcc 13.3.0 (`-std=c2x`) and clang 18.1.3 and
 * 19 (`-std=c23`), probed SEPARATELY, all compile and run every term below.
 *
 * ★★ EVERY VALUE HERE IS EVEN, AND THAT IS THE WHOLE DESIGN. A truncating `(_Bool)`
 * and a correct `!= 0` agree on every ODD value and disagree on every nonzero EVEN
 * one, so an odd probe is VACUOUS — it passes on a broken build. Each term below
 * therefore uses a nonzero value whose LOW BIT is 0 (and, for the wide and floating
 * sources, whose low LIMB or integral low bits are 0 too), which is exactly the
 * cell where the two implementations differ.
 *
 * The source categories are the C scalar family (C 6.2.5p21 — arithmetic ∪ pointer),
 * each in its own function so no frame grows large:
 *   t1 integer (the plain `int` case, plus a value that does not FIT in `_Bool`)
 *   t2 floating (`0.5` is nonzero and truncates to 0 — a `(_Bool)(int)d` route
 *      answers false; C compares the VALUE against 0, not its integer part)
 *   t3 pointer (null vs non-null, no arithmetic involved)
 *   t4 enum (projects to its underlying integer first)
 *   t5 a wide `__int128` whose LOW LIMB is entirely zero
 *   t6 `_BitInt` — narrow (its container's low bit) and wide (its low limb)
 *   t7 the cast used where its RESULT is consumed: a ternary condition, an
 *      arithmetic operand, and a returned value — the positions that made the
 *      defect reachable from ordinary code.
 *
 * exit = t1+t2+t3+t4+t5+t6+t7 = 7 * 6 = 42. */

typedef unsigned __int128 U128;

/* Opaque launderers so no const-fold answers these at compile time. */
static int    opi(int x)       { return x; }
static double opd(double x)    { return x; }
static U128   opw(U128 x)      { return x; }

/* t1 — INTEGER. 256 is nonzero and its low 8 bits are ZERO, so a `Trunc` to a
 * byte-wide `_Bool` answers false; 2 is nonzero with low bit 0, so even a 1-bit
 * truncation answers false. Both must be TRUE. */
static int t1(void) {
    if ((_Bool)opi(256) != 1) return 0;
    if ((_Bool)opi(2)   != 1) return 0;
    if ((_Bool)opi(0)   != 0) return 0;
    if ((_Bool)opi(-2)  != 1) return 0;
    return 6;
}

/* t2 — FLOATING. `0.5` is NONZERO but its integer part is 0, so any route through
 * an integer conversion answers false. C 6.3.1.2 compares the VALUE with 0. */
static int t2(void) {
    if ((_Bool)opd(0.5)  != 1) return 0;
    if ((_Bool)opd(2.0)  != 1) return 0;
    if ((_Bool)opd(0.0)  != 0) return 0;
    if ((_Bool)opd(-0.5) != 1) return 0;
    return 6;
}

/* t3 — POINTER. Nothing about a pointer's low bits carries its truth value. */
static int t3(void) {
    static int slot;
    int *const p = &slot;
    int *const n = (int *)0;
    if ((_Bool)p != 1) return 0;
    if ((_Bool)n != 0) return 0;
    return 6;
}

/* t4 — ENUM. Projects to its underlying integer, then the same `!= 0` test. The
 * enumerator chosen is EVEN and nonzero. */
enum E { EZero = 0, ETwo = 2, EBig = 256 };
static int t4(void) {
    enum E const a = (enum E)opi(2);
    enum E const b = (enum E)opi(256);
    enum E const z = (enum E)opi(0);
    if ((_Bool)a != 1) return 0;
    if ((_Bool)b != 1) return 0;
    if ((_Bool)z != 0) return 0;
    return 6;
}

/* t5 — a WIDE `__int128` whose LOW LIMB IS ENTIRELY ZERO. A conversion that reads
 * limb 0 alone answers false; the truth value is TRUE. */
static int t5(void) {
    U128 const hiOnly = opw((U128)3u << 64);
    U128 const zero   = opw((U128)0u);
    if ((_Bool)hiOnly != 1) return 0;
    if ((_Bool)zero   != 0) return 0;
    return 6;
}

/* t6 — `_BitInt`, NARROW and WIDE. The narrow one holds 2 (low bit 0, container
 * truncation says false); the wide one has bit 70 set, so LIMB 0 is entirely zero. */
static int t6(void) {
    _BitInt(17) const narrow = (_BitInt(17))opi(2);
    unsigned _BitInt(80) const wide = (unsigned _BitInt(80))opi(1) << 70;
    if ((_Bool)narrow != 1) return 0;
    if ((_Bool)wide   != 1) return 0;
    if ((_Bool)(_BitInt(17))opi(0) != 0) return 0;
    return 6;
}

/* t7 — the cast's RESULT CONSUMED, in the positions that make it reachable from
 * ordinary code: a ternary condition, an arithmetic operand, and a return value. */
static _Bool retBool(int v) { return (_Bool)v; }
static int t7(void) {
    int const v = opi(256);
    int const viaTernary = (_Bool)v ? 1 : 0;
    int const viaArith   = (_Bool)v + (_Bool)opi(2);   /* 1 + 1 */
    if (viaTernary != 1) return 0;
    if (viaArith   != 2) return 0;
    if (retBool(v) != 1) return 0;
    if (retBool(0) != 0) return 0;
    return 6;
}

int main(void) {
    return t1() + t2() + t3() + t4() + t5() + t6() + t7();
}
