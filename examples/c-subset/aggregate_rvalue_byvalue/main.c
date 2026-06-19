/* D-CSUBSET-BITFIELD-RVALUE-RUNTIME (FC8 SUBSTRATE) end-to-end RUNTIME witness
 * across all targets. Closes the aggregate-rvalue-carrier anchor: a by-value
 * COMPOUND LITERAL `(struct S){…}` used as an rvalue (passed BY VALUE, RETURNED
 * BY VALUE, or ASSIGNED from a literal) is an lvalue (C11 6.5.2.5p4) whose
 * storage is a synthesized slot the compiler now materializes + inits in place,
 * then consumes by ADDRESS — exactly like a named local's `= {…}`. The gap was
 * GENERAL, not bit-field-specific: an aggregate `ConstructAggregate` rvalue had
 * no lvalue address at all, so EVERY by-value compound literal (bit-field AND
 * non-bit-field, arg/return/assign) fail-louded at `lowerLvalueAddress`.
 *
 * This corpus exercises the new carrier across the full matrix:
 *   - a NON-bit-field struct compound literal in ARG, RETURN, and ASSIGN-from-
 *     literal positions;
 *   - a BIT-FIELD struct compound literal in the SAME three positions — the
 *     literal's slot init routes through lowerBitfieldAggregateInitIntoSlot, so
 *     each field PACKS per allocation unit (a:3 + b:5 share unit 0). Includes a
 *     SIGNED bit-field (s:4) holding a negative value that must SIGN-EXTEND on
 *     read — the most delicate path through the packer + reader.
 *
 * ANTI-FOLD: every literal's operands are derived from a MUTABLE GLOBAL
 * (`g_seed`) loaded at runtime, so ConstFold/Mem2Reg cannot collapse the
 * computation to a literal `return 42` — the slot materialization + per-field
 * stores (and bit-field RMW packing) run at runtime in BOTH the baseline and
 * the optimized arms. (A mutable global's load is opaque to value-propagation.)
 *
 * EXIT ARITHMETIC (each sub-result is verified against its runtime expectation;
 * ANY miscompile flips one equality and the exit to 13, never 42):
 *   g_seed = 5.
 *   non-bf ARG    : take_np((struct P){g, g+2})       = {5,7}  -> a+b      = 12
 *   non-bf RETURN : mk_np(g) = (struct P){g, g*3}     = {5,15} -> a+b      = 20
 *   non-bf ASSIGN : np = (struct P){g, g+10}          = {5,15} -> a+b      = 20
 *   bit-field ARG : take_bp((struct B){g, g+15, -3})  a=5,b=20,s=-3 (packed,
 *                                                      signed) -> a+b+s    = 22
 *   bit-field RET : mk_bp(g) = (struct B){g-4, g+3,-1} a=1,b=8, s=-1
 *                                                      -> a+b+s           = 8
 *   bit-field ASGN: bp = (struct B){g-2, g, -2}        a=3,b=5, s=-2
 *                                                      -> a+b+s           = 6
 *   ok = all six equal their runtime expectations; return ok ? 42 : 13.
 * A wrong slot/offset (non-bf), a bit-field pack that clobbers a neighbour, a
 * mis-extracted field, or a missing sign-extension flips one sum -> 13. The
 * exit is LAYOUT-RULE-AGNOSTIC (write+read share the gnu_packed rule, correct
 * on every target regardless of ABI-exactness — D-CSUBSET-BITFIELD-ABI-EXACT).
 * The optimizedPipelines arm proves the flow survives Mem2Reg/ConstFold; the
 * baseline arm is the real-codegen memory witness. arm64 runs under qemu; macho
 * on the macos-latest leg. */
struct P {
    int a;
    int b;
};

struct B {
    unsigned a : 3;   /* unit 0, bits 0..2          */
    unsigned b : 5;   /* unit 0, bits 3..7 (packs)  */
    int      s : 4;   /* unit 0, bits 8..11; signed */
};

/* Mutable global seed — its load is opaque to ConstFold/Mem2Reg, so the
 * compound literals below are materialized into slots at RUNTIME. */
int g_seed = 5;

/* By-value consumers: each takes the aggregate by value (the caller passes a
 * compound-literal rvalue, whose address is the synthesized slot). */
int take_np(struct P p) { return p.a + p.b; }
int take_bp(struct B b) { return (int)b.a + (int)b.b + (int)b.s; }

/* By-value producers: each RETURNS a compound-literal rvalue by value. */
struct P mk_np(int x) { return (struct P){x, x * 3}; }
struct B mk_bp(int x) { return (struct B){x - 4, x + 3, -1}; }

int main(void) {
    int g = g_seed;   /* runtime-opaque */

    /* non-bit-field: ARG, RETURN, ASSIGN-from-literal */
    int np_arg = take_np((struct P){g, g + 2});      /* {5,7}  -> 12 */
    struct P r = mk_np(g);                            /* {5,15}      */
    int np_ret = r.a + r.b;                           /*        -> 20 */
    struct P np;
    np = (struct P){g, g + 10};                       /* {5,15}      */
    int np_asn = np.a + np.b;                         /*        -> 20 */

    /* bit-field: ARG, RETURN, ASSIGN-from-literal (each PACKS per unit) */
    int bp_arg = take_bp((struct B){g, g + 15, -3});  /* a5 b20 s-3 -> 22 */
    struct B rb = mk_bp(g);                            /* a1 b8  s-1      */
    int bp_ret = (int)rb.a + (int)rb.b + (int)rb.s;   /*            -> 8  */
    struct B bp;
    bp = (struct B){g - 2, g, -2};                    /* a3 b5  s-2      */
    int bp_asn = (int)bp.a + (int)bp.b + (int)bp.s;   /*            -> 6  */

    int ok = (np_arg == (g + (g + 2)))
          && (np_ret == (g + g * 3))
          && (np_asn == (g + (g + 10)))
          && (bp_arg == (g + (g + 15) + (-3)))
          && (bp_ret == ((g - 4) + (g + 3) + (-1)))
          && (bp_asn == ((g - 2) + g + (-2)));
    return ok ? 42 : 13;
}
