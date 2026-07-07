/* c71 (D-CSUBSET-32BIT-ALU-FORMS): a SUB-INT (`char` / `signed char`) operand
 * of a truth-value condition (coerceCondition), of ++/-- (incDecArithValue), or
 * of a logical `!` (combineUnaryOp) is integer-PROMOTED to `int` (C 6.3.1.1 /
 * 6.5.3.3p5) BEFORE the ICmp / Sub, instead of forming a Char/I8-width op that
 * walls at the target's sub-native ALU gap ("integer TypeKind ordinal 15 has no
 * native-width ALU forms on target").
 *
 * c69 cleared sqlite's last crash and c70 cleared the big VDBE switch, so
 * sqlite3.c compiled END-TO-END and hit this x902 — a text library scans,
 * classifies, and steps `char` values on nearly every line. The ordinary
 * binary combine site already promotes (`a < b`, `c - '0'` widen fine); the
 * THREE sites exercised below (a condition, `++`/`--`, and logical `!`) build
 * the ICmp/Sub DIRECTLY and bypassed it.
 *
 * VALUE-CORRECTNESS ACROSS char SIGNEDNESS: DSS types plain `char` as SIGNED
 * (Char->int is SExt) uniformly; gcc uses signed char on x86_64 but UNSIGNED
 * char on arm64. So EVERY observable here is deliberately independent of
 * plain-char signedness (no plain-char `< 0`, no plain-char wraparound); the
 * two signedness assertions use `signed char` explicitly, where DSS and gcc
 * agree on all legs.
 *
 * SHAPE: one function of SEQUENTIAL loops (live ranges die between them) so it
 * stays under the pre-existing x86_64 register-pressure cliff when the small
 * static helpers would otherwise inline into one over-pressured `main` under
 * the release optimizer. Sums to 42. */

static int char_alu(void) {
    int total = 0;

    /* (A) coerceCondition on plain `char`: a bare `char` truth value — sqlite's
     * canonical NUL-terminated scan, `while (*z)`, is everywhere. */
    const char *z = "hello";
    while (*z) {                    /* Ne(char, 0): promoted -> ICmp i32 */
        total = total + 1;
        z = z + 1;
    }                               /* +5  -> 5  */

    /* (A) again + a single char equality (the compare already promotes at the
     * binary combine site; ASCII input keeps it signedness-agnostic). */
    const char *w = "banana";
    while (*w) {                    /* (A) plain-char condition */
        if (*w == 'a') total = total + 1;
        w = w + 1;
    }                               /* +3  -> 8  (a a a) */

    /* (B) incDecArithValue on plain `char`: `c++`, bounded so no wraparound ->
     * signedness-agnostic. Exercises the Char (ordinal 15) Add that walled. */
    char c = 0;
    while (c < 10) {                /* comparison promotes (combine site) */
        c++;                        /* (B) Char Add: promoted -> i32, trunc back */
        total = total + 1;
    }                               /* +10 -> 18 */

    /* (B) on `signed char`: `c++` THROUGH the signed 8-bit boundary. 127 -> -128
     * is unambiguous on every platform, pinning that promote->add->truncate-back
     * preserves 8-bit wraparound EXACTLY: C's `(signed char)((int)127 + 1)`. */
    signed char s = 126;
    s++;                            /* 127 */
    s++;                            /* -128 (signed 8-bit wrap) */
    if (s == -128) total = total + 1;   /* +1  -> 19 */

    /* (A) on `signed char` + a SExt signedness assertion. `(signed char)0x80`
     * is -128 on all platforms. `if (hi)` is (A) on I8 (nonzero -> +10). The
     * `hi < 0` compare is TRUE only under SExt promotion (-128 < 0); a wrong
     * ZExt would make it 128 < 0 = false. */
    signed char hi = (signed char)0x80;   /* -128 */
    if (hi) total = total + 10;     /* (A): -128 nonzero -> +10 -> 29 */
    if (hi < 0) total = total + 7;  /* SExt: -128 < 0 TRUE -> +7  -> 36 */

    /* (A') logical `!` on a plain `char` — the pervasive `if (!*z)` end-of-string
     * idiom. `!E` is `(E == 0)` (C 6.5.3.3p5), so the operand integer-promotes
     * exactly like a condition; BOTH truth outcomes are exercised (the false arm
     * must NOT fire). Pre-c71 this built a Char-typed ICmpEq that walled. */
    const char *e = "";             /* *e == '\0' == 0 */
    if (!*e) total = total + 6;     /* !0 == 1 -> +6 -> 42 */
    char nz = 5;
    if (!nz) total = total + 100;   /* !5 == 0 -> NOT added (the false arm) */

    return total;                   /* 42 */
}

int main(void) { return char_alu(); }
