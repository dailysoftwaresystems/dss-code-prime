/* D-CSUBSET-BITFIELD (FC8 cycle 2) end-to-end RUNTIME witness across all targets.
 * Exercises every bit-field form whose correctness is observable at runtime:
 *   - unsigned bit-field read + write (a, b, c);
 *   - ADJACENT PACKING in one allocation unit (a:3 + b:5 = 8 bits share a unit);
 *   - a SIGNED bit-field that sign-extends on read (s:4 holds -3 -> reads -3);
 *   - a ZERO-WIDTH unnamed bit-field (`: 0`) forcing `c` to a fresh unit;
 *   - NEIGHBOUR PRESERVATION: the ordinary `pad` field is written FIRST (=1000);
 *     a bit-field write is a read-modify-write, so it must NOT clobber `pad`
 *     (a plain over-wide store would) — `pad` stays 1000.
 *
 * a=5, b=20, s=-3, c=33, pad=1000:
 *   (int)a + (int)b + s + (int)c + (pad-1000) - 13
 *   = 5 + 20 + (-3) + 33 + 0 - 13 = 42.
 * A wrong extract/insert, mis-packing, missing sign-extension, or a clobbered
 * neighbour flips the exit code. The exit is LAYOUT-RULE-AGNOSTIC (write+read use
 * the same packing, so it is correct on every target regardless of whether our
 * gnu_packed rule byte-matches each platform's native compiler — see
 * D-CSUBSET-BITFIELD-ABI-EXACT). The optimizedPipelines arm proves the flow
 * survives Mem2Reg/ConstFold (the baseline arm is the real-codegen witness). */
struct Flags {
    unsigned a : 3;     /* unit 0, bits 0..2  */
    unsigned b : 5;     /* unit 0, bits 3..7 (packs with a) */
    int      s : 4;     /* signed bit-field */
    unsigned   : 0;     /* zero-width: force the next field to a new unit */
    unsigned c : 6;     /* a fresh unit after the break */
    int      pad;       /* an ordinary neighbour field */
};

int main(void) {
    struct Flags f;
    f.pad = 1000;       /* neighbour written FIRST */
    f.a = 5;
    f.b = 20;
    f.s = -3;           /* sign-extends to -3 on read */
    f.c = 33;
    return (int)f.a + (int)f.b + f.s + (int)f.c + (f.pad - 1000) - 13;
}
