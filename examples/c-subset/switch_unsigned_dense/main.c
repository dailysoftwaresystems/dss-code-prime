/* D-OPT-SWITCH-JUMP-TABLE (c70) UNSIGNED-discriminant witness: a dense switch
   on an `unsigned` value [0..15] (16 cases -> jump table). Exercises the ZExt
   widen (an unsigned discriminant must ZERO-extend, not sign-extend). The key
   probe: a HIGH-BIT-SET unsigned value (0x80000000u = 2147483648) must route to
   DEFAULT — a sign-extend bug would compute idx = (sext)0x80000000 - 0 = a
   negative-looking huge value that still lands out of range here, but the ZExt
   path is the CORRECT one per C's unsigned semantics. gcc/clang exit 42. */

int pick(unsigned n) {
    switch (n) {
        case 0u: return 200;
        case 1u: return 201;
        case 2u: return 202;
        case 3u: return 203;
        case 4u: return 204;
        case 5u: return 205;
        case 6u: return 206;
        case 7u: return 207;
        case 8u: return 208;
        case 9u: return 209;
        case 10u: return 210;
        case 11u: return 211;
        case 12u: return 212;
        case 13u: return 213;
        case 14u: return 214;
        case 15u: return 215;
        default: return -1;
    }
}

int main(void) {
    if (pick(0u)          != 200) return 1;   /* first */
    if (pick(15u)         != 215) return 2;   /* last  */
    if (pick(8u)          != 208) return 3;   /* interior */
    if (pick(16u)         != -1)  return 4;   /* above max -> default */
    if (pick(0x80000000u) != -1)  return 5;   /* high-bit-set -> default (ZExt) */
    if (pick(0xFFFFFFFFu) != -1)  return 6;   /* max unsigned -> default */
    return 42;
}
