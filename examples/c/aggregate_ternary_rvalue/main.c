/* D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR (FC8 SUBSTRATE) end-to-end RUNTIME
 * witness across all targets. Closes the aggregate-VALUED-control-expr carrier:
 * an AGGREGATE-typed `Ternary` (`cond ? A : B`) or comma/`SeqExpr` rvalue used
 * BY VALUE is a DIFFERENT carrier than the compound-literal slot — each
 * control-flow arm must be materialized into ONE COMMON result slot under a
 * CFG diamond (CondBr -> arm-init-slot -> join), then consumed by ADDRESS. There
 * is NO Phi of the aggregate (the compiler's aggregate model is memory-based:
 * the "merge" is a shared memory location, not an SSA Phi).
 *
 * This corpus exercises the new carrier across the full matrix, with EVERY
 * ternary's condition + arm value derived from a RUNTIME-opaque source (the
 * mutable global `g_seed`, loaded once — its load is opaque to ConstFold/Mem2Reg
 * — plus the runtime parameter `pick`) so neither arm folds away and BOTH the
 * true AND the false arm are witnessed:
 *   - a NON-bit-field struct ternary in ARG position, witnessed BOTH ways via
 *     two calls with different RUNTIME pick values (pick_t derived true, pick_f
 *     derived false) — the result reflects WHICH arm was selected, so a
 *     wrong-arm / mis-selection flips the exit;
 *   - a NON-bit-field struct ternary in ASSIGN-from-rvalue position;
 *   - a BIT-FIELD struct ternary in RETURN position — each arm packs per
 *     allocation unit through the common slot (a:3 + b:5 share unit 0, plus a
 *     SIGNED s:4 holding a negative value that must SIGN-EXTEND on read). This
 *     proves the per-unit packing flows through the diamond-materialized slot;
 *   - a comma/`SeqExpr` whose VALUE is an aggregate rvalue, used BY VALUE — the
 *     side effect (a LOCAL counter bump) must run, then the aggregate result is
 *     consumed by value;
 *   - a NON-bit-field struct ternary whose ARMS are NAMED LVALUES (not compound
 *     literals) in ARG position — each arm is COPIED into the common slot
 *     (the copy-path: lowerLvalueAddress(arm) + aggregate copy through the
 *     diamond), distinct from the in-place compound-literal init above.
 *
 * ANTI-FOLD: every condition + arm field value derives from `g_seed` (a global
 * LOAD, opaque to value-propagation) and runtime `pick` parameters, so the
 * diamond + per-arm slot materialization (and bit-field RMW packing) run at
 * runtime in BOTH the baseline and the optimized arms. A runtime condition keeps
 * both arms live. (Note: NO global is WRITTEN — global loads are runtime-opaque,
 * which is all the anti-fold needs.)
 *
 * EXIT ARITHMETIC (each sub-result is verified against its runtime expectation;
 * ANY miscompile — wrong arm, wrong slot/offset, a bit-field pack that clobbers
 * a neighbour, a mis-extracted field, a missing sign-extension, or a dropped
 * comma side effect — flips one equality and the exit to 13, never 42):
 *   g_seed = 5; pick_t = (g_seed > 0) = 1; pick_f = (g_seed < 0) = 0.
 *
 *   non-bf ARG, pick TRUE  : sel_np(pick_t, g) chooses then arm {g,g+2}
 *                            = {5,7}    -> a+b = 12
 *   non-bf ARG, pick FALSE : sel_np(pick_f, g) chooses else arm {g+100,g}
 *                            = {105,5}  -> a+b = 110
 *   non-bf ASSIGN (true)   : np = pick_t ? (struct P){g,g*3} : (struct P){0,0}
 *                            = {5,15}   -> a+b = 20
 *   bit-field RETURN (true) : mk_bp(g,pick_t) = pick ? {g,g+15,-3} : {0,0,0}
 *                            a=5,b=20,s=-3 (signed) -> a+b+s = 22
 *   comma/SeqExpr ARG      : take_np( (bump += 1, (struct P){g, g+4}) )
 *                            = {5,9}  side-effect ran -> a+b = 14, bump == 1
 *   named-arm ARG (copy)   : take_np(pick_t ? na : nb) chooses na = {g,g+6}
 *                            = {5,11} -> a+b = 16  (each arm COPIED into slot)
 *
 *   ok = all six equal their runtime expectations AND the comma side effect
 *   was observed once; return ok ? 42 : 13.
 *
 * The exit is LAYOUT-RULE-AGNOSTIC (write+read share the gnu_packed rule, so it
 * is correct on every target regardless of ABI-exactness —
 * D-CSUBSET-BITFIELD-ABI-EXACT). The optimizedPipelines arm proves the flow
 * survives Mem2Reg/ConstFold; the baseline arm is the real-codegen memory
 * witness. arm64 runs under qemu; macho on the macos-latest leg. */
struct P {
    int a;
    int b;
};

struct B {
    unsigned a : 3;   /* unit 0, bits 0..2          */
    unsigned b : 5;   /* unit 0, bits 3..7 (packs)  */
    int      s : 4;   /* unit 0, bits 8..11; signed */
};

/* Mutable global seed — its LOAD is opaque to ConstFold/Mem2Reg, so the ternary
 * conditions stay runtime (both arms live) and the arm field values are
 * materialized into the common slot at RUNTIME. It is only ever READ. */
int g_seed = 5;

/* By-value consumer: takes the aggregate by value (the caller passes the
 * ternary/comma rvalue, whose address is the synthesized common slot). */
int take_np(struct P p) { return p.a + p.b; }

/* A NON-bit-field aggregate ternary in ARG position, behind a runtime `pick`
 * parameter (so the same call site is witnessed BOTH ways from main). */
int sel_np(int pick, int g) {
    return take_np(pick ? (struct P){g, g + 2} : (struct P){g + 100, g});
}

/* By-value producer returning a BIT-FIELD aggregate ternary by value. */
struct B mk_bp(int x, int pick) {
    return pick ? (struct B){x, x + 15, -3} : (struct B){0, 0, 0};
}

int main(void) {
    int g      = g_seed;        /* runtime-opaque global load */
    int pick_t = (g_seed > 0);  /* runtime true  (no fold; depends on the load) */
    int pick_f = (g_seed < 0);  /* runtime false                                */

    /* non-bit-field ternary in ARG position, witnessed BOTH ways */
    int np_arg_t = sel_np(pick_t, g);   /* then arm {5,7}    -> 12  */
    int np_arg_f = sel_np(pick_f, g);   /* else arm {105,5}  -> 110 */

    /* non-bit-field ternary in ASSIGN-from-rvalue position (then arm) */
    struct P np;
    np = pick_t ? (struct P){g, g * 3} : (struct P){0, 0};   /* {5,15}      */
    int np_asn = np.a + np.b;                                /*        -> 20 */

    /* bit-field ternary in RETURN position (then arm, packs through the slot) */
    struct B rb = mk_bp(g, pick_t);                          /* a5 b20 s-3   */
    int bp_ret = (int)rb.a + (int)rb.b + (int)rb.s;          /*        -> 22  */

    /* comma/SeqExpr whose VALUE is an aggregate rvalue, used BY VALUE: the
     * LOCAL `bump += 1` side effect must run, then the aggregate result {g,g+4}
     * is consumed by value. */
    int bump = 0;
    int sq_arg = take_np((bump += 1, (struct P){g, g + 4}));  /* {5,9} -> 14 */

    /* NAMED-LVALUE arms (the copy-path): a ternary selecting between two EXISTING
     * struct locals — each arm is COPIED into the common slot (lowerAggregateCopy
     * through the diamond), distinct from the in-place compound-literal init the
     * cases above exercise. */
    struct P na = {g, g + 6};                                /* {5,11}         */
    struct P nb = {g + 50, g};                               /* {55,5}         */
    int np_named = take_np(pick_t ? na : nb);                /* then {5,11}->16 */

    int ok = (np_arg_t == (g + (g + 2)))
          && (np_arg_f == ((g + 100) + g))
          && (np_asn   == (g + g * 3))
          && (bp_ret   == (g + (g + 15) + (-3)))
          && (sq_arg   == (g + (g + 4)))
          && (np_named == (g + (g + 6)))
          && (bump     == 1);
    return ok ? 42 : 13;
}
