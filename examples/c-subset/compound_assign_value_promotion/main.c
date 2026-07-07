/* c74 (D-CSUBSET-32BIT-ALU-FORMS): a compound assignment (`&=` `|=` `^=` `<<=`
 * `>>=` `+=` `-=` `*=`) on a SUB-INT lvalue used in VALUE / expression position
 * (`total += (x &= m)`, `if ((flags &= ~M))`, `while ((c |= b))`) must integer-
 * PROMOTE the base op to `int` (C99 `a OP= b` == `a = (T)((a) OP (b))`, the OP
 * computed at the COMMON type) — else it builds a Char/U8-typed BinaryOp that
 * walls at the target's sub-native ALU gate.
 *
 * c71+c72+c73 promoted the char/sub-int ALU at the condition / ++-- / unary /
 * bit-field sites; the STATEMENT-position compound assign (`x &= y;`) already
 * promoted via lowerCompoundAssign. But a VALUE-position compound assign lowers
 * through a DIFFERENT path (the lowerExpr work-stack driver's finishAssign +
 * the lowerBinary Assign arm) that built the BinaryOp at the LVALUE type with
 * un-promoted operands. This is the sqlite `if( (p->flags &= ~MASK) )` idiom —
 * the post-c73 re-probe's last 2 U8 errors (op#18 = And, operands U8,I32 -> U8).
 *
 * The VALUE of a compound assign is the STORED (truncated-to-lvalue-type) value,
 * so every observable here is the low-byte result — signedness-agnostic (unsigned
 * u8) or explicit `signed char`. Matches gcc on x86 + aarch64. => 42. */

typedef unsigned char u8;

int main(void) {
    int total = 0;

    u8 x = 0xFF;
    total += (x &= 0x0F);                 /* x = 0x0F = 15; value 15 -> 15 */

    u8 y = 5;
    total += (y <<= 2);                   /* y = 20; value 20 -> 35 (Shl on u8) */

    u8 z = 0xF0;
    if ((z &= 0x0F) == 0) total += 3;     /* z = 0; ==0 -> +3 -> 38 (And in a cond) */

    u8 w = 1;
    total += (w |= 2);                    /* w = 3; value 3 -> 41 (Or on u8) */

    signed char s = -8;
    total += ((s += 9) == 1) ? 1 : 0;     /* s = 1; ==1 -> +1 -> 42 (Add on signed char) */

    return total;                          /* 42 */
}
