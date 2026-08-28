/* D-CSUBSET-BITFIELD-INIT (FC8) end-to-end RUNTIME witness across all targets.
 * Aggregate INITIALIZATION of a bit-field struct (`struct S s = {…};`) — the
 * path that previously fail-louded (a plain field-wise init would write each
 * bit-field full-width into the shared allocation unit, clobbering its
 * neighbours). This corpus exercises EVERY init site that packs:
 *   - a GLOBAL bit-field struct initialized with `= {…}` (the static-data byte
 *     encoder packs each field into its unit at compile time);
 *   - a LOCAL bit-field struct initialized with `= {…}` (per-unit slot packing
 *     at run time — zero the unit, OR each field in);
 *   - both POSITIONAL `{…}` and DESIGNATED `{.f=…}` initializers;
 *   - an ANONYMOUS zero-width bit-field (`unsigned : 0;`) that a positional
 *     initializer must SKIP (C 6.7.9) — `c`'s value must land on `c`, not on
 *     the anonymous slot;
 *   - ADJACENT PACKING (a:3 + b:5 share one unit) read back correctly;
 *   - a SIGNED bit-field that sign-extends on read (s:4 holds -3 → reads -3);
 *   - NEIGHBOUR PRESERVATION inside a unit: initializing a, b, s (all in unit 0)
 *     must not clobber each other (each is a masked OR into the zeroed unit);
 *   - an ordinary field (`pad`) initialized alongside the bit-fields;
 *   - ORDINARY-FIELD-SHARES-A-BIT-FIELD-UNIT (F1, review-caught): a smaller
 *     ordinary field declared BEFORE a bit-field whose allocation unit overlaps
 *     it (`struct T { char x; unsigned a:3; ... }` — x at byte 0, a's int unit
 *     at bytes [0,4)). The unit zero-fill must NOT clobber x. Exercised as BOTH
 *     a global (`gt`, static-data encoder pre-zeroes the buffer once) and a
 *     local (`lt`, the MIR slot init must zero every unit in a pre-pass before
 *     writing any field), proving the two paths AGREE — a lazy per-unit zero in
 *     declaration order wiped the local's x (exit → 35) while the global stayed
 *     correct (the global/local divergence the two-pass fix removes).
 *
 * Per S, a=5,b=20,s=-3,c=33 → (int)a+(int)b+s+(int)c = 5+20-3+33 = 55.
 *   global gp (positional)  : 55
 *   global gd (designated)  : 55
 *   local  lp (positional)  : 55, plus pad set to 1000 then read back
 *   local  ld (designated)  : 55, with .pad=1000 in the initializer
 * Per T, x=7,a=5,b=9 → (int)x+(int)a+(int)b = 21; gt + lt = 42.
 * sum of the four 55s = 220; (ld.pad - lp.pad) = 0; tv = 42; 220 + 42 - 220 = 42.
 * A wrong pack, a missing anon-skip, a mis-extracted field, a missing sign-
 * extension, a clobbered neighbour, or a clobbered ordinary-field-in-unit flips
 * the exit. The exit is LAYOUT-RULE-AGNOSTIC (write+read share the gnu_packed
 * rule — correct on every target regardless of ABI-exactness; see
 * D-CSUBSET-BITFIELD-ABI-EXACT). arm64 runs under qemu; macho on the
 * macos-latest leg. */
struct S {
    unsigned a : 3;     /* unit 0, bits 0..2  */
    unsigned b : 5;     /* unit 0, bits 3..7 (packs with a) */
    int      s : 4;     /* unit 0, bits 8..11 — signed bit-field */
    unsigned   : 0;     /* anonymous zero-width: positional init must SKIP it */
    unsigned c : 6;     /* a fresh unit after the break */
    int      pad;       /* an ordinary neighbour field */
};

struct T {
    char     x;         /* byte 0 — an ordinary field SMALLER than a's unit */
    unsigned a : 3;     /* int unit [0,4), bits 8..10 — SHARES x's byte-0 unit */
    unsigned b : 4;     /* same unit, bits 11..14 */
};

struct S gp = { 5, 20, -3, 33 };                          /* global, positional */
struct S gd = { .a = 5, .b = 20, .s = -3, .c = 33 };      /* global, designated */
struct T gt = { 7, 5, 9 };       /* global F1: x must survive the unit pack */

int main(void) {
    struct S lp = { 5, 20, -3, 33 };                      /* local, positional */
    lp.pad = 1000;
    struct S ld = { .a = 5, .b = 20, .s = -3, .c = 33, .pad = 1000 };
    struct T lt = { 7, 5, 9 };    /* local F1: pre-pass zero must not wipe x */

    int gpv = (int)gp.a + (int)gp.b + gp.s + (int)gp.c;   /* 55 */
    int gdv = (int)gd.a + (int)gd.b + gd.s + (int)gd.c;   /* 55 */
    int lpv = (int)lp.a + (int)lp.b + lp.s + (int)lp.c;   /* 55 */
    int ldv = (int)ld.a + (int)ld.b + ld.s + (int)ld.c;   /* 55 */
    int tv  = (int)gt.x + (int)gt.a + (int)gt.b           /* 21 (global) */
            + (int)lt.x + (int)lt.a + (int)lt.b;          /* 21 (local)  → 42 */

    return gpv + gdv + lpv + ldv + (ld.pad - lp.pad) + tv - 220;
}
