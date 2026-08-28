/* C23 6.7.2.1 — `_BitInt(N)` as a bit-field MEMBER type (D-CSUBSET-BITINT-BITFIELD,
 * the C4a arc-closer sub-cycle). A `_BitInt(N)` bit-field of width M <= N is stored in
 * M bits; a READ yields that M-bit value interpreted as `_BitInt(N)` — SIGNED fields
 * sign-extend, UNSIGNED fields zero-extend (the base's signedness drives it, via the
 * container repr, exactly like the shipped enum bit-field). The allocation unit is the
 * `_BitInt`'s native container (I8/U8..I64/U64); a base > 32 bits uses a 64-bit unit.
 * Wide (N>64) `_BitInt` bit-fields are rejected at the semantic tier (the >64 cap),
 * not exercised here.
 *
 * RED-ON-DISABLE: exit 42 depends on (a) the SIGNED 4-bit field sign-extending its -3
 * (a non-sign-extending extract reads 13 -> exit 26), (b) the UNSIGNED 4-bit field
 * zero-extending, and (c) the 33-bit field surviving in a 64-bit unit (a 32-bit-only
 * store loses bit 32 of 5e9 -> the equality fails -> exit 16). Operands are `volatile`
 * seeded so no pass can const-fold the store/load away; the release arm re-checks. */

struct S {
    unsigned _BitInt(8)  u : 4;    /* 4-bit unsigned field, sub-int (u8) unit */
    signed   _BitInt(8)  s : 4;    /* 4-bit signed field: sign-extends -8..7 */
    unsigned _BitInt(40) w : 33;   /* 33-bit field: needs the 64-bit unit (>32) */
};

int main(void) {
    volatile int su = 13, ss = -3;
    volatile unsigned long long sw = 5000000000ULL;   /* 5e9: needs bit 32 */

    struct S a;
    a.u = (unsigned _BitInt(8))su;    /* 13 -> 4-bit field */
    a.s = (signed _BitInt(8))ss;      /* -3 -> signed 4-bit field */
    a.w = (unsigned _BitInt(40))sw;   /* 5e9 -> 33-bit field */

    int r = (int)a.u;                                /* 13  (unsigned extract) */
    r -= (int)a.s;                                   /* 13 - (-3) = 16 (sign-extend) */
    r += (a.w == 5000000000ULL) ? 26 : 0;            /* + 26 (33-bit read intact) */
    return r;                                        /* 13 + 3 + 26 = 42 */
}
