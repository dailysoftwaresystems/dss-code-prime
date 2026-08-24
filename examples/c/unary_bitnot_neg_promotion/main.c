/* c72 (D-CSUBSET-32BIT-ALU-FORMS): unary `~` (BitNot) and `-` (Neg) on a
 * SUB-INT operand integer-PROMOTE the operand to `int` (C 6.5.3.3p3/p4 — the
 * integer promotions are performed on the operand, and the result has the
 * PROMOTED type). c71 promoted the condition / ++-- / `!` sites; the post-c71
 * re-probe then showed sqlite's `~u8`/`-u8` flag math STILL walling at the
 * sub-native ALU gap ("integer TypeKind ordinal 6 [U8] / 7 [U16] has no
 * native-width ALU forms" — sqlite's `u8`/`u16` typedefs). The FIX generalizes
 * c71's combineUnaryOp promotion to Not/BitNot/Neg; unlike `!` (whose result is
 * Bool), `~`/`-` carry the PROMOTED (int) result type so an assignment back to
 * a narrow lvalue truncates via the normal coerce.
 *
 * Signedness: u8/u16 promote via ZExt, signed char via SExt. Every observable
 * here is either masked to the low byte (sqlite's `x & ~mask` idiom, which is
 * signedness-agnostic) or uses `signed char` explicitly, so gcc agrees on all
 * legs (DSS types char signed uniformly; gcc uses unsigned char on arm64).
 * Sequential statements keep it under the -O register-pressure cliff. => 42. */

typedef unsigned char  u8;
typedef unsigned short u16;

static int unary_sub_int(void) {
    int total = 0;

    /* `~u8` (BitNot, ZExt promote) then truncate back — the sqlite flag idiom
     * `x & ~mask`. (u8)~(u8)0xF0 = (u8)(~240) = (u8)0xFFFFFF0F = 0x0F = 15.
     * Pre-c72 the `~` built a Char/U8-typed MIR Not that walled. */
    u8 m = 0xF0;
    total = total + (u8)~m;              /* 15 -> 15 */

    /* `~u16` (BitNot, ZExt) masked to the low byte: ~(u16)0xFFF0 = -65521 =
     * 0xFFFF000F; & 0xFF = 0x0F = 15. */
    u16 w = 0xFFF0;
    total = total + ((~w) & 0xFF);       /* 15 -> 30 */

    /* `-u8` (Neg, ZExt): -(u8)1 promotes to -(int)1 = -1 (the result is the
     * PROMOTED int type, so the compare against -1 is a plain i32 compare). */
    u8 one = 1;
    if (-one == -1) total = total + 5;   /* +5 -> 35 */

    /* `~i8` (BitNot, SExt on signed char): ~(signed char)0 = ~0 = -1. Pins the
     * SExt promotion direction for a signed sub-int operand. */
    signed char z = 0;
    if (~z == -1) total = total + 7;     /* +7 -> 42 */

    return total;                        /* 42 */
}

int main(void) { return unary_sub_int(); }
