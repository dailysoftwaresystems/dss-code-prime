/* D-CSUBSET-ENUM-BITFIELD (FC8 cycle 5) end-to-end RUNTIME witness.
 * An ENUM-TYPED bit-field (`enum Color c : 4;`) is permitted (C 6.7.2.1) — an
 * enum behaves AS its underlying integer (D-CSUBSET-ENUM-INT-CONVERSION), so a
 * bit-field on an enum validates its width against the underlying and lowers its
 * allocation-unit access (load/store width, shift/mask constants, SIGNEDNESS) at
 * that underlying integer.
 *
 * The load-bearing witness is SIGNEDNESS: this v1's enum underlying is signed
 * `int`, so an enum bit-field must SIGN-EXTEND on read. `NEG = -3` stored into a
 * 4-bit enum bit-field is the bit pattern 1101; reading it back must yield -3
 * (signed extract: Shl+AShr), NOT 13 (unsigned extract: LShr+And). That choice
 * is made at MIR-lowering time by `bitfieldIsSigned`, which must see the enum's
 * UNDERLYING kind — the whole point of the cycle's enumReprType resolve.
 * RED-ON-DISABLE: revert the emitBitfieldExtract enum→underlying resolve and the
 * two enum bit-fields holding NEG read back as 13 → exit 74, not 42. (The LIR
 * width tier `reprKind` already masks the load/store WIDTH, so signedness is the
 * arm a runtime witness can actually discriminate; the const/op REPRESENTATION
 * is pinned by the MIR unit test EnumTypedBitFieldResolvesAndLowers.)
 *
 * This corpus exercises: TWO enum bit-fields packing in one unit (c:4 + d:4), a
 * GLOBAL enum-bitfield init (g), a LOCAL init + WRITES (`l.c = NEG;`) + READS, a
 * SIGNED enum value via a negative enumerator, and a signed `int` neighbour (s).
 *
 * enum Color { NEG = -3, GREEN = 5, BLUE = 6 };
 * global g = { NEG, BLUE, -2 } : g.c=-3, g.d=6, g.s=-2          → -3+6-2 = 1
 * local  l written { NEG, BLUE, -1 } : l.c=-3, l.d=6, l.s=-1    → -3+6-1 = 2
 * exit = 1 + 2 + 39 = 42. LAYOUT-RULE-AGNOSTIC (write+read share gnu_packed).
 * arm64 runs under qemu; macho on the macos-latest leg. */
enum Color { NEG = -3, GREEN = 5, BLUE = 6 };

struct S {
    enum Color c : 4;   /* unit 0, bits 0..3  — enum bit-field (signed underlying) */
    enum Color d : 4;   /* unit 0, bits 4..7  — packs with c */
    int        s : 4;   /* unit 0, bits 8..11 — signed neighbour */
};

struct S g = { NEG, BLUE, -2 };     /* global enum-bitfield init; g.c = -3 */

int main(void) {
    struct S l = { GREEN, GREEN, 0 };
    l.c = NEG;                       /* write a negative enumerator → sign-extends on read */
    l.d = BLUE;
    l.s = -1;
    return (int)g.c + (int)g.d + (int)g.s    /* -3 + 6 + (-2) = 1 */
         + (int)l.c + (int)l.d + (int)l.s    /* -3 + 6 + (-1) = 2 */
         + 39;                                /* → 42 */
}
