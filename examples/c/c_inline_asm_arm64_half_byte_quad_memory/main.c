/* THE HALF-, BYTE- AND QUAD-WIDTH SIMD&FP MEMORY FORMS ON aarch64, END TO END
 * AND BY EXECUTION — the runnable half of
 * D-TARGET-ARM64-HALF-BYTE-QUAD-MEMORY-FORMS-UNDECLARED.
 *
 * ★★★ WHAT THIS PROVES THAT A BYTE PIN CANNOT. The defect had two halves and
 * they failed differently: `arm64.target.json` declared `fldur`/`fstur`/
 * `fldr_u`/`fstr_u` at widths 32/64/128 only, so `ldr h0` and `ldr b0` were
 * refused with *no candidate target opcode encodes that shape*; and the SIMD&FP
 * register table spelled `v`/`d`/`s`/`h`/`b` and not `q`, so `ldr q0` was
 * refused at the register LOOKUP. ✔MEASURED at the P55 base through the CLI,
 * one inline-`asm` template per compile, with `ldr s0,[x9,#4]` and
 * `ldr d0,[x9,#8]` compiling rc=0 beside them as the controls.
 *
 * ★★★ AND THE FAILURE THIS FILE EXISTS FOR IS A WRONG ADDRESS, NOT A REFUSAL.
 * The scaled unsigned-offset LDR/STR encodes `imm12 = byteOffset / accessSize`,
 * so an arm that borrowed a neighbour's scale does not fail — it assembles, it
 * runs, and it reads SOMEWHERE ELSE. ✔MEASURED, gas 2.42 and clang 18.1.3
 * agreeing: `[x1,#16]` is imm12 16 at B, 8 at H, 4 at S, 2 at D and 1 at Q —
 * five fields for one offset. Every shape below therefore COPIES a slice out of
 * a byte-distinct source buffer and reads the destination back in C: a
 * mis-scaled displacement lands on different bytes, and different bytes are
 * exactly what the comparisons see.
 *
 * ★★ THE WIDTH IS FENCED AS WELL AS THE ADDRESS. Each destination slice is
 * checked with its NEIGHBOURS still zero, so a 2-byte store that ran as a
 * 4- or 8-byte one is caught by the byte after it rather than by the bytes
 * it was supposed to write.
 *
 * ★★★ THE `q0` / `v0` IDENTITY IS PROVED BY EXECUTION, not by inspection.
 * `q0` is not a register row: it is an ALIAS SPELLING of `v0`, and both names
 * resolve to ONE ordinal. `dssQIsV` below writes the register through its `q`
 * spelling, moves it as a VECTOR (`mov v0.16b, v1.16b`, the ORR alias), and
 * stores it through the `q` spelling again — three instructions that only agree
 * about which machine register they mean if the alias resolves to `v`. It also
 * carries `"q0"` in its CLOBBER list, which is the other side of the same
 * claim: a clobber is canonicalised to an ORDINAL, so an alias that minted a
 * second row would reserve a register the allocator never hands out while `v0`
 * stayed free — a silent wrong-register answer with nothing to see.
 *
 * ⚠ THE BASE POINTER IS MOVED INTO A PHYSICAL REGISTER FIRST, and that is a
 * property of the DIALECT rather than a style choice: ✔MEASURED, this dialect's
 * memory production takes a register NAME inside its brackets, so a template
 * placeholder there (`ldr h0, [%0, #2]`) is a parse error
 * (`P_NoAlternativeMatched … expected 'Identifier', 'IntLiteral' or
 * 'MinusSign' — got '%'`) for the INTEGER control too. `x9`/`x10` are
 * clobbered accordingly.
 *
 * ⚠ EVERY BUFFER IS `volatile` SO THE RELEASE ARM STILL REACHES THE TEMPLATES —
 * a folded read would let this file pass without executing anything.
 *
 * Exit codes name the failing shape; 42 means every one held.
 */

#if defined(__aarch64__)

/* Byte-distinct on purpose: every value is unique across the whole buffer, so
 * a slice copied from the WRONG address cannot coincide with the right one. */
static volatile unsigned char dssSrc[64] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40
};

/* ⚠ NINETY-SIX BYTES AND EVERY SHAPE OWNS A DISJOINT SLICE, which is a
 * correction rather than a choice: written at 64 bytes, shape (6)'s
 * destination overlapped shape (1)'s and the cross-width comparison at the end
 * read bytes a later shape had overwritten (✔MEASURED under qemu-aarch64: exit
 * 20, with `aarch64-linux-gnu-objdump` confirming every emitted word was the
 * right one). A fixture that overwrites its own evidence reports a compiler
 * defect it did not find. */
static volatile unsigned char dssDst[96];

/* ── the SCALED unsigned-offset forms (`fldr_u` / `fstr_u`) ─────────────── */

/* LDR Ht / STR Ht — accessSize 2. A scale of 4 would read `src+8` and a scale
 * of 8 would read `src+4`; both are legal instructions and both copy the wrong
 * two bytes. */
static void dssCopyHalfScaled(volatile unsigned char* s,
                              volatile unsigned char* d) {
    __asm__ volatile("mov x9, %x0\n\t"
                     "mov x10, %x1\n\t"
                     "ldr h0, [x9, #16]\n\t"
                     "str h0, [x10, #2]"
                     : : "r"(s), "r"(d) : "memory", "x9", "x10", "v0");
}

/* LDR Bt / STR Bt — accessSize 1, the one width at which the field and the
 * byte offset coincide. Included precisely because a scale bug is INVISIBLE
 * here and visible everywhere else: it is the control for the other two. */
static void dssCopyByteScaled(volatile unsigned char* s,
                              volatile unsigned char* d) {
    __asm__ volatile("mov x9, %x0\n\t"
                     "mov x10, %x1\n\t"
                     "ldr b0, [x9, #5]\n\t"
                     "str b0, [x10, #9]"
                     : : "r"(s), "r"(d) : "memory", "x9", "x10", "v0");
}

/* LDR Qt / STR Qt — accessSize 16, the mainstream NEON memory idiom. The `q0`
 * SPELLING is the whole reason this shape exists. */
static void dssCopyQuadScaled(volatile unsigned char* s,
                              volatile unsigned char* d) {
    __asm__ volatile("mov x9, %x0\n\t"
                     "mov x10, %x1\n\t"
                     "ldr q0, [x9, #16]\n\t"
                     "str q0, [x10, #32]"
                     : : "r"(s), "r"(d) : "memory", "x9", "x10", "q0");
}

/* ── the UNSCALED signed-imm9 forms (`fldur` / `fstur`) ─────────────────── */

/* ★ NEGATIVE DISPLACEMENTS AT EVERY WIDTH, which the scaled twin cannot
 * express at all — so an `fldur` arm accidentally wired to the scaled slot
 * fails loud here instead of passing quietly. */
static void dssCopyHalfUnscaled(volatile unsigned char* s,
                                volatile unsigned char* d) {
    __asm__ volatile("mov x9, %x0\n\t"
                     "mov x10, %x1\n\t"
                     "ldur h0, [x9, #-2]\n\t"
                     "stur h0, [x10, #-4]"
                     : : "r"(s), "r"(d) : "memory", "x9", "x10", "v0");
}

static void dssCopyByteUnscaled(volatile unsigned char* s,
                                volatile unsigned char* d) {
    __asm__ volatile("mov x9, %x0\n\t"
                     "mov x10, %x1\n\t"
                     "ldur b0, [x9, #-3]\n\t"
                     "stur b0, [x10, #-7]"
                     : : "r"(s), "r"(d) : "memory", "x9", "x10", "v0");
}

static void dssCopyQuadUnscaled(volatile unsigned char* s,
                                volatile unsigned char* d) {
    __asm__ volatile("mov x9, %x0\n\t"
                     "mov x10, %x1\n\t"
                     "ldur q0, [x9, #-16]\n\t"
                     "stur q0, [x10, #-16]"
                     : : "r"(s), "r"(d) : "memory", "x9", "x10", "q0");
}

/* ── the alias, by execution ────────────────────────────────────────────── */

/* Load through `q1`, move as a VECTOR through `v0`/`v1`, store through `q0`.
 * The three instructions name the same two machine registers only if `q_k`
 * resolves to `v_k`; if the alias resolved anywhere else the stored bytes are
 * whatever that register happened to hold. */
static void dssQIsV(volatile unsigned char* s, volatile unsigned char* d) {
    __asm__ volatile("mov x9, %x0\n\t"
                     "mov x10, %x1\n\t"
                     "ldr q1, [x9, #32]\n\t"
                     "mov v0.16b, v1.16b\n\t"
                     "str q0, [x10, #48]"
                     : : "r"(s), "r"(d)
                     : "memory", "x9", "x10", "q0", "v1");
}

int main(void) {
    volatile unsigned char* const s = dssSrc;
    volatile unsigned char* const d = dssDst;

    for (int i = 0; i < 96; ++i) d[i] = 0;

    /* (1) LDR Ht / STR Ht: src[16..17] -> dst[2..3]. */
    dssCopyHalfScaled(s, d);
    if (d[2] != s[16]) return 1;
    if (d[3] != s[17]) return 2;
    if (d[1] != 0)     return 3;   /* the store was 2 bytes wide, not more */
    if (d[4] != 0)     return 4;

    /* (2) LDR Bt / STR Bt: src[5] -> dst[9]. */
    dssCopyByteScaled(s, d);
    if (d[9] != s[5])  return 5;
    if (d[8] != 0)     return 6;
    if (d[10] != 0)    return 7;

    /* (3) LDR Qt / STR Qt: src[16..31] -> dst[32..47]. */
    dssCopyQuadScaled(s, d);
    for (int i = 0; i < 16; ++i) {
        if (d[32 + i] != s[16 + i]) return 8;
    }
    if (d[31] != 0) return 9;      /* sixteen bytes, and not one more */
    if (d[48] != 0) return 10;

    /* (4) LDUR Ht / STUR Ht, both displacements NEGATIVE:
     *     (src+8)-2 = src[6..7]  ->  (dst+20)-4 = dst[16..17]. */
    dssCopyHalfUnscaled(s + 8, d + 20);
    if (d[16] != s[6]) return 11;
    if (d[17] != s[7]) return 12;
    if (d[15] != 0)    return 13;
    if (d[18] != 0)    return 14;

    /* (5) LDUR Bt / STUR Bt: (src+8)-3 = src[5] -> (dst+28)-7 = dst[21]. */
    dssCopyByteUnscaled(s + 8, d + 28);
    if (d[21] != s[5]) return 15;
    if (d[20] != 0)    return 16;
    if (d[22] != 0)    return 17;

    /* (6) LDUR Qt / STUR Qt: (src+48)-16 = src[32..47] -> (dst+80)-16
     *     = dst[64..79]. */
    dssCopyQuadUnscaled(s + 48, d + 80);
    for (int i = 0; i < 16; ++i) {
        if (d[64 + i] != s[32 + i]) return 18;
    }
    if (d[63] != 0) return 19;
    if (d[80] != 0) return 20;

    /* (7) THE ALIAS: src[32..47] -> (dst+32)+48 = dst[80..95] via
     *     q1 -> v1 -> v0 -> q0. */
    dssQIsV(s, d + 32);
    for (int i = 0; i < 16; ++i) {
        if (d[80 + i] != s[32 + i]) return 21;
    }
    /* And it stayed inside its slice: the byte below it is still the one
     * shape (6) put there. */
    if (d[79] != s[47]) return 22;

    /* (8) THE SCALE DISCRIMINATOR, stated as an assertion rather than left
     *     implicit: shapes (1) and (3) wrote the SAME source offset (#16) at
     *     TWO widths, and the bytes they produced must agree at the overlap.
     *     An H arm scaled like a D arm would have read src[4..5] in (1) while
     *     (3) still read src[16..], so the two would disagree here. */
    if (d[2] != d[32]) return 23;
    if (d[3] != d[33]) return 24;

    return 42;
}

#else

/* Not an aarch64 target: this example's subject is the aarch64 SIMD&FP memory
 * forms and the `q` register spelling. It is registered for the arm64 specs
 * only (see expected.json), so this arm exists to keep the file a legal
 * translation unit rather than to assert anything. */
int main(void) { return 42; }

#endif
