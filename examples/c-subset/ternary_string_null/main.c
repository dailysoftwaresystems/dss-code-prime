/* c66 (D-CSUBSET-TERNARY-NULL-STRING-LITERAL): a ternary with a STRING-LITERAL
 * arm and a null-pointer-constant (literal-0) arm — `cond ? "%s" : 0` — decays
 * the string literal to char* (C 6.3.2.1p3) and treats 0 as a null pointer (C
 * 6.5.15p6), so the result is `char*` (a pointer), NOT an aggregate. Frontier
 * sqlite `sqlite3VdbeError(..., (sParse.zErrMsg ? "%s" : 0), sParse.zErrMsg)`
 * (sqlite3.c:161910). The c49 null-ptr rule needs the non-0 arm already a Ptr;
 * the c64 array-decay needs BOTH arms arrays — NEITHER handles `array : 0`, so
 * the fallback typed it Array<char,3> and the aggregate-ternary lowering then
 * tried to string-materialize the literal-0 arm → H0009 (the Literal pool entry
 * is an int, not a string; D-LK4-RODATA-PRODUCER-NONSTRING-ARRAY-LITERAL-DECAY).
 * The fix adds an Array-arm + literal-0 arm to BOTH combineTernary tiers →
 * Ptr<elem>. RED-ON-DISABLE: revert the c66 arms → H0009 on the first form.
 *
 * VALUE-correct: the selected arm's bytes are read through the decayed pointer,
 * and the null arm is checked == 0. */

int keep(int x){ return x; }                 /* keep conditions runtime-opaque */

char *take(char *f){ return f; }             /* a char* PARAM (sqlite's fmt arg) */

char *fmt(int has){ return has ? "%s" : 0; } /* the return form */

int main(void){
    /* (1) the EXACT sqlite shape: `cond ? "%s" : 0` as a char* CALL ARG */
    char *a1 = take(keep(1) ? "%s" : 0);     /* -> "%s" */
    char *a0 = take(keep(0) ? "%s" : 0);     /* -> NULL */
    if (a1 == 0 || a1[0] != '%' || a1[1] != 's' || a1[2] != 0) return 1;
    if (a0 != 0) return 2;                    /* the null-pointer arm */

    /* (2) the return form */
    char *r1 = fmt(keep(1));                  /* "%s" */
    char *r0 = fmt(keep(0));                  /* NULL */
    if (r1 == 0 || r1[0] != '%' || r1[2] != 0) return 3;
    if (r0 != 0) return 4;

    /* (3) reverse arm order: `cond ? 0 : "..."` */
    char *v1 = (keep(0) ? 0 : "err");         /* "err" */
    char *v0 = (keep(1) ? 0 : "err");         /* NULL */
    if (v1 == 0 || v1[0] != 'e' || v1[1] != 'r' || v1[2] != 'r' || v1[3] != 0) return 5;
    if (v0 != 0) return 6;

    return 42;
}
