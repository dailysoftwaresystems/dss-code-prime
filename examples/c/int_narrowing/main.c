/* Cluster-F F4 (int_narrowing): C 6.3.1.3 / 6.5.16.1 same-signedness implicit
 * integer NARROWING — the gate `intSameSignednessNarrows`. This is a hard SQLite
 * blocker: int<->short<->char assignments are everywhere in real C.
 *
 * Every narrowing here is from a RUNTIME (non-literal, non-inlined-param) source,
 * so (a) the optimizer cannot fold it to a destination-width literal that would
 * compile even with the gate OFF, and (b) it is a real RED-ON-DISABLE: with
 * intSameSignednessNarrows=false the program fails to COMPILE (S0003), so the
 * `release` arm's green is an end-to-end witness that the gate admits the
 * conversion AND coerce()/mapCast TRUNCATES it correctly.
 *
 * Coverage:
 *   - narrowing in all four assignment positions: INIT, ASSIGN, CALL-ARG, RETURN
 *   - TRUNCATION witnessed (the result is the low-N-bits value, not the full int)
 *   - integer PROMOTION still computes in int (char+char does NOT overflow narrow)
 * exit 42 == every conversion correct on this target.
 */

/* INIT position: int -> short. 0x12345678 truncates to low 16 bits 0x5678. */
int narrow_init_short(int x)     { short s = x; return s; }

/* ASSIGN position: int -> signed char. 300 truncates to low 8 bits 0x2C = 44. */
int narrow_assign_schar(int x)   { signed char c = 0; c = x; return c; }

/* CALL-ARG position: an int argument passed to a short parameter truncates. */
int take_short(short s)          { return s; }
int narrow_arg_short(int x)      { return take_short(x); }

/* RETURN position: an int expression returned from a short-returning function. */
short narrow_return_short(int x) { return x; }

/* PROMOTION (C 6.3.1.1) must still hold: each operand widens to int before the
 * add, so the sum does NOT wrap in the narrow type. */
int promote_uchar(unsigned char a, unsigned char b) { return a + b; }  /* narrow u8 -> 144 */
int promote_schar(signed char a, signed char b)     { return a + b; }  /* narrow i8 -> -56 */

int main(void) {
    /* runtime locals (real values across the non-inlined call boundary) */
    int big    = 0x12345678;     /* low16 = 0x5678 = 22136 */
    int n300   = 300;            /* low8  = 0x2C   = 44     */
    int n65549 = 0x10000 + 13;   /* 0x1000D, low16 = 13     */
    int n70000 = 70000;          /* 0x11170, low16 = 0x1170 = 4464 */
    int u200   = 200;
    int s100   = 100;

    /* ---- narrowing in every assignment position, truncation witnessed ---- */
    if (narrow_init_short(big)      != 0x5678) return 1;   /* INIT   */
    if (narrow_assign_schar(n300)   != 44)     return 2;   /* ASSIGN */
    if (narrow_arg_short(n65549)    != 13)     return 3;   /* ARG    */
    if (narrow_return_short(n70000) != 4464)   return 4;   /* RETURN */

    /* ---- promotion still computes in int (would wrap if done in the narrow type) ---- */
    if (promote_uchar(u200, u200)   != 400)    return 5;   /* not 144 */
    if (promote_schar(s100, s100)   != 200)    return 6;   /* not -56 */

    return 42;
}
