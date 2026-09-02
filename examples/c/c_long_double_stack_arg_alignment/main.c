/* D-CSUBSET-LONG-DOUBLE-STACK-ARG-ALIGNMENT: runtime witness that the CALLER's
 * outgoing placement of a stacked `long double` and the CALLEE's incoming read of
 * it land on the SAME byte when the ABI requires a 16-byte alignment pad.
 *
 * THE SHAPE THAT MAKES THE PAD OBSERVABLE. On x86_64 SysV the first six integer
 * arguments take RDI/RSI/RDX/RCX/R8/R9, so `a7` is the FIRST stack argument at
 * outgoing offset 0 and the `long double` that follows it sits at an ODD 8-byte
 * cursor. The x87 ABI puts that datum on the 16-byte boundary — outgoing +16, NOT
 * +8 — and gcc 13.3.0 and clang 18.1.3 both read it with `fldt 24(%rsp)`
 * (incoming +16) for exactly this signature (✔MEASURED 2026-09-02).
 *
 * ★ WHY THIS EXAMPLE IS WORTH RUNNING EVEN THOUGH DSS CALLS DSS. The pad is
 * inserted by TWO different walks — `StackArgCursor` on the caller's side and the
 * incoming-parameter cursor in HIR→MIR on the callee's — and a fix applied to one
 * of them and not the other reads the long double eight bytes away from where it
 * was written. This program returns 42 only if the two agree; it is the pin
 * against a HALF-applied alignment rule, which is the failure mode that would
 * otherwise be invisible until a foreign boundary. The FOREIGN half (a DSS caller
 * against a gcc-built callee) is measured out-of-tree, since DSS has no
 * long-double libm FFI surface to link against in the corpus.
 *
 * `even8` is the CONTROL: an EVEN preceding stack-argument count already leaves
 * the cursor 16-aligned, so no pad is owed and the same datum lands at +16 from a
 * different arithmetic. Both must hold, or the pad is unconditional rather than
 * alignment-driven.
 *
 * ⚠ WHICH LEG THIS WITNESS ACTUALLY REDDENS ON, stated because the answer is not
 * "all of them" and a reader who assumes otherwise would over-trust one gate.
 * ✔MEASURED: with the callee-side round-up deleted the emitted ELF reads the home
 * at `lea 0x68(%rsp)` instead of `lea 0x70(%rsp)` and EXECUTING it returns 4 (the
 * `(int)ld != 20` arm) against the unmutated 42 — so this program genuinely
 * detects a half-applied rule. But it detects it only where the ELF arm is
 * SPAWNED: on a Windows host the runner compiles a `runOn: ["linux"]` arm and
 * skips its run, and the arm that DOES spawn there is pe64, whose axis collapses
 * `long double` to binary64 so no datum out-aligns the slot. The Windows leg's
 * red-on-disable therefore lives in the unit pin
 * (tests/lir/test_lir_long_double_stack_arg_alignment.cpp), which reads the
 * shipped config directly; this example is the LINUX-leg execution witness.
 *
 * The other three long-double axes reach here too and are all correct by
 * construction: pe64 x86_64 and Apple arm64 collapse `long double` to binary64
 * (an 8-byte datum never out-aligns the slot), and AAPCS64 passes binary128 in a
 * v-register rather than on the stack. So this witness is a genuine x86_64-ELF
 * ALIGNMENT test and a genuine no-op everywhere else — which is the blast radius
 * the change claims.
 *
 * ANTI-FOLD: every operand flows through a MUTABLE GLOBAL (the c11_atomic
 * precedent), so the `release` arm proves a real runtime call boundary rather
 * than a folded constant.
 *
 * exit = 42.
 */

long double g_ld;
long        g_seven;
long        g_eight;

/* SEVEN integer parameters: a1..a6 fill the SysV GPR argument registers, a7 is
 * the first STACK argument, and `ld` therefore follows at an ODD 8-byte cursor —
 * the case that owes a 16-byte alignment pad. */
int odd7(long a1, long a2, long a3, long a4, long a5, long a6,
         long a7, long double ld) {
    if (a1 != 1 || a2 != 2 || a3 != 3) return 1;
    if (a4 != 4 || a5 != 5 || a6 != 6) return 2;
    if (a7 != 7)                       return 3;
    /* 20.5L truncates to 20. A read eight bytes away from the write yields the
     * neighbouring slot's bytes reinterpreted as an x87 extended value, which is
     * not 20 — so this compare is the caller/callee agreement assertion. */
    if ((int)ld != 20)                 return 4;
    return 42;
}

/* The CONTROL: EIGHT integer parameters leave the cursor already 16-aligned, so
 * the same datum lands at the same +16 with NO pad inserted. */
int even8(long a1, long a2, long a3, long a4, long a5, long a6,
          long a7, long a8, long double ld) {
    if (a7 != 7 || a8 != 8) return 5;
    if ((int)ld != 20)      return 6;
    return 42;
}

int main(void) {
    g_ld    = 20.5L;
    g_seven = 7;
    g_eight = 8;

    int const oddResult = odd7(1, 2, 3, 4, 5, 6, g_seven, g_ld);
    if (oddResult != 42) {
        return oddResult;
    }
    int const evenResult = even8(1, 2, 3, 4, 5, 6, g_seven, g_eight, g_ld);
    if (evenResult != 42) {
        return evenResult;
    }
    return 42;
}
