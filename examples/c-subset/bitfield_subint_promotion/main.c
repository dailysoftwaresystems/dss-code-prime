/* c73 (D-CSUBSET-32BIT-ALU-FORMS): a bit-field whose allocation UNIT is a
 * SUB-INT type (u8/u16/signed char) must compute its extract/insert arithmetic
 * (shift + mask + and/or) at the PROMOTED int width, not the sub-int unit
 * width — otherwise the synthesized ops wall at the target's sub-native ALU
 * gate ("integer TypeKind ordinal 6 [U8] / 7 [U16] has no native-width ALU
 * forms"). c71+c72 promoted the char/sub-int ALU at the source-operator sites
 * (condition, ++/--, !, ~, -); the post-c72 sqlite3.c re-probe then showed
 * sqlite's pervasive `u8 flag:1` bit-field structs still walling, because the
 * bit-field read/write is SYNTHESIZED at emitBitfieldExtract/Insert (hir_to_mir)
 * on the u8 storage unit, bypassing combineBinary's promotion.
 *
 * FIX: emitBitfieldExtract/Insert promote a sub-int unit to i32 for the RMW
 * arithmetic, then Trunc back to the unit type for the caller/Store. A NATIVE
 * unit (>=32-bit, incl. the wide `u64:40` case) is unchanged. Verified vs gcc
 * on x86 (signed char) + aarch64 (unsigned char) — the observables are either
 * signedness-agnostic (unsigned u8/u16 fields, positive values) or use `signed
 * char` explicitly. Exercises: read, write, RMW neighbour-preservation, 3-bit
 * overflow truncation, signed sign-extension, aggregate init, u16 unit. => 42. */

typedef unsigned char  u8;
typedef unsigned short u16;

struct Flags { u8 a:3; u8 b:5; };      /* two fields packed in ONE u8 unit */
struct Init  { u8 p:2; u8 q:6; };
struct Wide  { u16 x:10; u16 y:6; };   /* u16 unit */
struct Sgn   { signed char s:4; };     /* signed 4-bit field -> sign-extend */

int main(void) {
    int total = 0;

    struct Flags f;
    f.a = 5; f.b = 20;
    total += f.a + f.b;                          /* 5 + 20 = 25 */
    f.a = 7;                                     /* RMW: b (same unit) preserved */
    total += (f.b == 20) ? 3 : 0;                /* +3  -> 28 */
    f.a = 9;                                     /* 3-bit overflow: 9 & 7 = 1 */
    total += f.a;                                /* +1  -> 29 */

    struct Sgn sg; sg.s = -3;                    /* 4-bit signed */
    total += (sg.s == -3) ? 4 : 0;               /* +4  -> 33 (SExt of the field) */

    struct Init i = { 2, 10 };                   /* aggregate init of a bit-field unit */
    total += (i.p == 2 && i.q == 10) ? 5 : 0;    /* +5  -> 38 */

    struct Wide w; w.x = 1000; w.y = 40;         /* u16 unit, two fields */
    total += (w.x == 1000 && w.y == 40) ? 4 : 0; /* +4  -> 42 */

    return total;                                /* 42 */
}
