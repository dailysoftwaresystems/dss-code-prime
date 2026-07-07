/* c64 (D-CSUBSET-TERNARY-ARRAY-DECAY, C 6.3.2.1p3 + 6.5.15): a conditional with
 * two STRING-LITERAL (array) arms decays EACH arm to `char*`, so the result is a
 * pointer — not an aggregate. Frontier sqlite3.c:83149 (sqlite3BtreeIntegrity-
 * Check / checkAppendMsg):
 *   checkAppendMsg(pCheck, "%s is %u but should be %u",
 *                  isFreeList ? "size" : "overflow list length", ...);
 * The ternary is the FIRST vararg (matching %s) of a printf-style variadic
 * function — a variadic arg has NO param type, so there is no return/assignment
 * coercion to decay it. Without the array-decay arms the conditional types as
 * `char[5]` (aggregate) and reaches lowerExpr as a bare aggregate rvalue, which
 * a phi cannot represent -> H0009 (D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR). The
 * fix adds the array-decay arm to BOTH combineTernary tiers (semantic_analyzer +
 * cst_to_hir): both string-literal arms decay to the common `char*`, and the HIR
 * coerce() Array->Ptr arm materializes a GlobalAddr to each rodata string.
 * RED-ON-DISABLE: revert the array-decay arms -> the variadic form (1) below
 * fails H0009 at compile (the exact sqlite error).
 *
 * VALUE-correct: every selected arm's bytes are read THROUGH the decayed
 * pointer, so a wrong arm / wrong decay miscompares. */

#include <stdarg.h>

int keep(int x){ return x; }                 /* keep conditions runtime-opaque */

/* mirrors checkAppendMsg(IntegrityCk*, const char *zFormat, ...): the ternary
 * arrives as the FIRST vararg, retrieved as a char*. */
char *grab(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    char *p = va_arg(ap, char *);
    va_end(ap);
    return p;
}

/* the return-form: a `char*` function returning the conditional. Without decay
 * the return-coercion routes the char[5]-typed conditional through the
 * by-address aggregate lowering into a char[5] slot -> a truncated, DANGLING
 * pointer (a silent miscompile the decay also prevents). */
char *pick(int isFreeList){
    return isFreeList ? "size" : "overflow list length";
}

int main(void){
    /* (1) the EXACT sqlite shape — the ternary as a variadic %s arg */
    char *v1 = grab("%s", keep(1) ? "size" : "overflow list length");  /* "size" */
    char *v0 = grab("%s", keep(0) ? "size" : "overflow list length");  /* long arm */
    if (v1[0] != 's' || v1[3] != 'e' || v1[4] != 0)   return 1;
    if (v0[0] != 'o' || v0[8] != ' ' || v0[9] != 'l') return 2;       /* "overflow l…" */

    /* (2) the return-form, read through the returned pointer (string literals
     * live in rodata, so the pointer is valid after pick returns) */
    char *r1 = pick(keep(1));                                          /* "size" */
    char *r0 = pick(keep(0));                                          /* "overflow list length" */
    if (r1[0] != 's' || r1[4] != 0)                    return 3;
    if (r0[0] != 'o' || r0[19] != 'h' || r0[20] != 0)  return 4;       /* "…length\0" */

    /* (3) inline assignment, both arm orders */
    char *a = (keep(0) ? "yes" : "no");                               /* "no"   */
    char *b = (keep(1) ? "ABCD" : "z");                              /* "ABCD" */
    if (a[0] != 'n' || a[1] != 'o' || a[2] != 0)       return 5;
    if (b[0] != 'A' || b[3] != 'D' || b[4] != 0)       return 6;

    return 42;
}
