/* C23 _BitInt(N) objects with STATIC STORAGE DURATION (D-CSUBSET-BITINT-DATA-GLOBAL).
 *
 * Before this example DSS REFUSED every initialized `_BitInt` global — a scalar one
 * at `lowerMirGlobalsToDataItems`' explicit deferral wall ("`_BitInt` DATA-globals
 * are not yet emitted"), and a `_BitInt` STRUCT MEMBER at the aggregate-leaf
 * recursion, whose sole scalar encoder returns a `std::uint64_t` and so structurally
 * cannot carry an N>64 image. Only a TENTATIVE `_BitInt(17) g;` worked (it reserves
 * .bss through the layout engine and never encodes bytes). clang 18.1.3 / 19
 * `-std=c23` compile and RUN every declaration below; gcc 13.3.0 `-std=c2x` has no
 * `_BitInt` at all (it rejects the declaration itself), so under
 * `DSS = (gcc u clang u MSVC) u ISO C` this is required by the clang half.
 *
 * ★★ THE PADDING BITS ARE THE POINT OF `g_neg` AND `g_s.a`, AND THEY ARE A DECISION.
 * C23 6.2.6.2 gives a `_BitInt(N)` object N value bits inside a sizeof-byte container;
 * C23 6.2.6.1p6 leaves the remaining PADDING bits' values UNSPECIFIED. clang zero-fills
 * them in a static image (MEASURED: `_BitInt(17) = -3wb` emits `fd ff 01 00`); DSS
 * SIGN-EXTENDS them, because that is the invariant its own `bitIntMask`/`maskTopLimb`
 * wrap chokepoint establishes at runtime (MEASURED by execution: a runtime
 * `_BitInt(17) = -3` has byte 2 == 0xff, and so does a runtime `_BitInt(65) = -1` at
 * byte 8). The static image MUST agree with the runtime that reads it: `g_neg < 0`
 * below is FALSE — the container would hold +131069 — under a zero-filling emitter.
 * That one comparison is the executable form of the decision.
 *
 * Covers every shape the two producers reach, because one gate refused them all:
 *   - narrow SIGNED negative scalar (.data)        g_neg    padding pin
 *   - narrow UNSIGNED 1-byte scalar (.data)        g_u8     width pin (1 byte, not 4)
 *   - const scalar (.rodata)                       g_ro
 *   - TENTATIVE zero-init (.bss)                   g_tent   regression pin
 *   - WIDE _BitInt(128), 2 limbs                   g_w128   VALUE BITS ABOVE BIT 64
 *   - WIDE _BitInt(200), 4 limbs, N%64 == 8        g_w200   32-byte image, top-limb mask
 *   - STRUCT with two _BitInt members              g_s      aggregate-leaf recursion
 *   - ARRAY of _BitInt(65)                         g_arr    16-byte element stride
 *   - UNION whose first member is _BitInt(100)     g_un     union-leaf, bit 99
 *   - _BitInt BIT-FIELDS in a static initializer   g_bf     the packer leaf
 *
 * RED-ON-DISABLE / why the exit DISCRIMINATES: every term is strictly positive and
 * every one reads bytes the image must carry, so losing any single one lowers the
 * total and moves the exit. Four terms read VALUE BITS ABOVE BIT 64 (g_w128 >> 64,
 * g_w200 >> 192, g_w200 >> 128, g_arr[i] >> 64, g_un.w >> 99): a LOW-LIMB-ONLY
 * emitter reads 0 for all of them. A byte-order error inside a limb moves the &255
 * terms; a LIMB-order error swaps the >>-terms with them; a container-width error on
 * g_u8 (4 bytes where 1 is reserved) overruns its item; a zero-filled padding flips
 * g_neg and g_s.a. `idx` comes from argc — an OS-supplied runtime value — so the
 * array reads survive the shipped release pipeline and the array bytes must really be
 * in the image. All widths are data-model-independent, so ONE exit code holds on all
 * four targets. arm64 runs under qemu; macho on the macos-latest leg. */

_Static_assert(sizeof(_BitInt(17))           ==  4, "_BitInt(17) is a 4-byte container");
_Static_assert(sizeof(unsigned _BitInt(8))   ==  1, "unsigned _BitInt(8) is 1 byte");
_Static_assert(sizeof(unsigned _BitInt(65))  == 16, "_BitInt(65) is ceil(65/64)*8 == 16");
_Static_assert(sizeof(unsigned _BitInt(200)) == 32, "_BitInt(200) is ceil(200/64)*8 == 32");

/* 1. NARROW SIGNED, NEGATIVE — the padding-bit pin (see the header). */
_BitInt(17) g_neg = -3wb;

/* 2. NARROW UNSIGNED — a ONE-byte container. A 4-byte write here would overrun the
 *    item the layout reserves, so "assume _BitInt is int-sized" cannot pass quietly. */
unsigned _BitInt(8) g_u8 = 200uwb;

/* 3. const → .rodata (a distinct section decision from g_neg's writable .data). */
const _BitInt(17) g_ro = 5wb;

/* 4. TENTATIVE → .bss, no on-disk bytes. This shape already worked; it is here as a
 *    regression pin, because the new scalar arm sits on the same dispatch. */
_BitInt(17) g_tent;

/* 5. WIDE, 2 limbs: 69*2^64 + 42. Limb 1 is 69 — ABOVE bit 64, so a low-limb-only
 *    image reads 0 for it; limb 0's low byte is 42. */
unsigned _BitInt(128) g_w128 = 1272825341085959061546uwb;

/* 6. WIDE, 4 limbs, N%64 == 8: 5*2^192 + 3*2^128. Bits 192..199 are the ONLY valid
 *    bits of the top limb, so this pins both the 32-byte image length and the
 *    top-limb mask; limb 2 carries 3. */
unsigned _BitInt(200) g_w200 =
    31385508676933403820199794216801147470901901044615477198848uwb;

/* 7. STRUCT — the aggregate-leaf recursion, with a signed member (padding again) and
 *    a 1-byte member that sits at offset 4 behind a 4-byte one. */
struct S { _BitInt(17) a; unsigned _BitInt(8) b; };
struct S g_s = { -3wb, 200uwb };

/* 8. ARRAY of a 16-byte element — the element STRIDE is load-bearing: element 0 has
 *    bit 64 set, element 1 does not, and their low bytes differ. */
unsigned _BitInt(65) g_arr[2] = { 18446744073709551639uwb, 29uwb };

/* 9. UNION whose first member is wide: 2^99 + 19. */
union U { unsigned _BitInt(100) w; };
union U g_un = { 633825300114114700748351602707uwb };

/* 10. BIT-FIELDS of `_BitInt` type in a STATIC initializer — the aggregate encoder's
 *     OTHER leaf, which packs into an allocation unit instead of writing an image.
 *     ✔MEASURED: this shape refused before P42 with the GENERIC aggregate text, and
 *     nothing named it — the RUNTIME twin (`examples/c/c23_bitint_bitfield`) has
 *     always worked, so only the static initializer was walled. Same field widths as
 *     that example, so the two tiers are compared on identical ground. */
struct BF {
    unsigned _BitInt(8)  u : 4;    /* 4-bit unsigned field, u8 unit */
    signed   _BitInt(8)  s : 4;    /* 4-bit SIGNED field: -3 stores as 0b1101 */
    unsigned _BitInt(40) w : 33;   /* 33-bit field: needs the 64-bit unit (>32) */
};
struct BF g_bf = { 13uwb, -3wb, 5000000000uwb };

int main(int argc, char **argv) {
    (void)argv;
    int const idx = argc - 1;            /* 0 — but the compiler cannot know that */

    /* ── the PADDING decision, executable ────────────────────────────────────────
     * -3 at _BitInt(17) is 0x1FFFD. With DSS's sign-extended padding the 4-byte
     * container holds 0xFFFFFFFD and reads back negative. With clang's zero-filled
     * padding it holds 0x0001FFFD == +131069 and this term is 0. */
    int const t1 = (g_neg < (_BitInt(17))0) ? 3 : 0;              /*   3 */
    int const t2 = (int)g_u8;                                     /* 200 */
    int const t3 = (int)g_ro;                                     /*   5 */
    int const t4 = (g_tent == (_BitInt(17))0) ? 7 : 0;            /*   7 */

    /* ── value bits ABOVE bit 64 ─────────────────────────────────────────────── */
    int const t5 = (int)(g_w128 >> 64);                           /*  69 */
    int const t6 = (int)(g_w128 & 255uwb);                        /*  42 */
    int const t7 = (int)(g_w200 >> 192);                          /*   5 */
    int const t8 = (int)((g_w200 >> 128) & 255uwb);               /*   3 */

    /* ── the aggregate-leaf recursion ────────────────────────────────────────── */
    int const t9  = (g_s.a < (_BitInt(17))0) ? 11 : 0;            /*  11 */
    int const t10 = (int)g_s.b;                                   /* 200 */

    /* ── array element stride + union first member ───────────────────────────── */
    int const t11 = (int)(g_arr[idx] >> 64);                      /*   1 */
    int const t12 = (int)(g_arr[idx] & 255uwb);                   /*  23 */
    int const t13 = (int)(g_arr[idx + 1] & 255uwb);               /*  29 */
    int const t14 = (int)(g_un.w >> 99);                          /*   1 */
    int const t15 = (int)(g_un.w & 255uwb);                       /*  19 */

    /* ── the STATIC bit-field packer ─────────────────────────────────────────────
     * t17 negates a SIGNED 4-bit field: a packer that stored the wrong low bits, or
     * a read that zero-extends, gives +13 instead of -3 and the term moves by 16.
     * t18's field is 33 bits wide, so a 32-bit-only unit loses bit 32 of 5e9. */
    int const t16 = (int)g_bf.u;                                  /*  13 */
    int const t17 = -(int)g_bf.s;                                 /*   3 */
    int const t18 = (g_bf.w == 5000000000uwb) ? 26 : 0;           /*  26 */

    /* 3+200+5+7+69+42+5+3+11+200+1+23+29+1+19+13+3+26 == 660; 660 - 618 == 42. */
    return t1 + t2 + t3 + t4 + t5 + t6 + t7 + t8 + t9
         + t10 + t11 + t12 + t13 + t14 + t15 + t16 + t17 + t18 - 618;
}
