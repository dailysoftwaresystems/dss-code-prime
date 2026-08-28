/* C23 _BitInt(N) arbitrary-magnitude wb/uwb LITERALS (D-CSUBSET-BITINT-WIDE-LITERAL,
 * the C4b arc-closer). A `wb`/`uwb` suffix gives an integer constant the bit-precise
 * type `[unsigned] _BitInt(N)` with N the smallest width that holds its value — and
 * the value may EXCEED 64 bits, so it decodes through a compile-time bignum and
 * materializes into the multi-limb (Model-A) value, ceil(N/64) little-endian i64 limbs.
 *
 * Here `633825300114114700748351602688uwb` is 2^99: its bit length is 100, so the
 * literal's type is `unsigned _BitInt(100)` (2 limbs) and its high limb (limb 1) MUST
 * carry 2^35 — a LOW-LIMB-ONLY literal materialization reads back 0. A `volatile` seed
 * makes `big` runtime-dependent so the release optimizer cannot fold the whole thing
 * away; the wide value is realized BY ADDRESS, so mem2reg/const-fold preserve the limb
 * fill and the exit stays 42.
 *
 * Exit 42 is load-bearing / RED-ON-DISABLE: `2^99 >> 93 == 2^6 == 64`, `2^99 & 1023 == 0`,
 * so `64 + 0 - 22 == 42`. Drop the high limb during materialization and `big` is 0 →
 * `0 >> 93 == 0` → exit ≠ 42.
 */

int main(void) {
    volatile int seed = 1;

    /* An arbitrary-magnitude uwb literal ( = 2^99 ), > 2^64 → needs limb 1. */
    unsigned _BitInt(100) big = 633825300114114700748351602688uwb;

    /* +0, but runtime (seed is volatile) so the optimizer keeps the wide value live. */
    big = big + (unsigned _BitInt(100))(seed - 1);

    unsigned _BitInt(100) top = big >> 93;     /* 2^99 >> 93 == 2^6 == 64 */
    unsigned _BitInt(100) low = big & 1023uwb; /* 2^99 & 1023 == 0 (narrow uwb literal) */

    int r = (int)top + (int)low - 22;          /* 64 + 0 - 22 == 42 */
    return r;
}
