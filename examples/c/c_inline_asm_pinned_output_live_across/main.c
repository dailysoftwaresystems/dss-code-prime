/* A REGISTER-PINNED ASM OUTPUT MUST NOT EAT A VALUE THAT MERELY LIVES ACROSS
 * THE BLOCK — D-LIR-PER-INSTRUCTION-OUTPUTS-NOT-ENFORCED-SUBSET-OF-CLOBBERED.
 *
 * ★★★ WHAT THIS EXAMPLE WITNESSES, AND WHY NO SIBLING COULD. The register
 * allocator's forbidden set at an instruction is `inputs ∪ clobbered`, and
 * OUTPUTS ARE DELIBERATELY OMITTED from it. For the instruction's OWN operands
 * that omission is sound on its own terms — the instruction reads its operands
 * before it writes its outputs, so an operand may share a register with an
 * output. For a value that merely LIVES ACROSS the instruction that argument
 * says nothing at all: such a value is not read by the instruction, and a
 * register the instruction writes destroys it. What makes the omission safe
 * THERE is a separate invariant — every output is also declared a clobber —
 * enforced by the `.target.json` loader for the per-OPCODE carrier and by
 * `LirBuilder::regConstraintPoolAdd` for the per-INSTRUCTION carrier an
 * `__asm__` statement fills.
 *
 * ⇒ this file is the RUNNABLE half of that invariant. The template pins BOTH
 * its outputs (`"=a"`, `"=d"`) and fourteen unrelated values are live across
 * it. If the pinned registers do not reach the forbidden set, two of those
 * fourteen are allocated to %rax / %rdx and the block overwrites them — rc=0,
 * no diagnostic, wrong answer.
 *
 * ✔MEASURED 2026-08-27, x86_64:pe64-x86_64-windows-exec, with the lowering's
 * output→clobber normalization AND both of its fail-loud layers removed:
 *     baseline → exit 42   (GREEN — see the arm note below)
 *     release  → exit  3   (RED: the crossing sum was 105 and came back wrong)
 * and, in the same mutant's disassembly of an earlier shape, the two pinned
 * outputs collided outright — `mov %rax,%rdx` followed by `mov %rdx,%r14`,
 * so the second output read the first one's value out of a register that had
 * already been overwritten.
 *
 * ★★ THE `release` ARM IS THE DISCRIMINATING ONE AND THAT IS A MEASUREMENT,
 * NOT AN ACCIDENT. At baseline the fourteen crossing values are memory-
 * resident, so no register the asm block writes can hold one and the mutant's
 * artifact is BYTE-IDENTICAL to the clean one. The same asymmetry is recorded
 * in `c_inline_asm_operands` for a different mutant. ⛔ Do not drop the
 * `release` arm as redundant — it is the only arm that reddens.
 *
 * ⚠ THE `volatile` SEED CARRIES THE LOAD. Each `dss_seed + k` is a volatile
 * READ, so the fourteen values can be neither folded nor common-subexpressed
 * nor recomputed after the block: they must survive IN REGISTERS (or spill),
 * which is what puts them in the allocator's way. Without it the optimizer
 * folds them to constants and rematerializes them after the asm, and a
 * destroyed register costs nothing.
 *
 * ⚠ FOURTEEN, NOT TWO. The count is what creates the pressure that makes the
 * allocator reach for %rax and %rdx at all; with a couple of crossing values
 * it simply picks elsewhere and the pin asserts nothing.
 *
 * ★ THE TEMPLATE WRITES CONSTANTS, NOT A COUNTER. An `rdtsc`-shaped subject
 * (the sibling `c_inline_asm_extended`) cannot assert what its outputs HOLD,
 * so a corruption between the two output registers is invisible to it. Here
 * `lo` must be 11 and `hi` must be 22 or the example says which one broke.
 */

#if !defined(__x86_64__)
#error "c_inline_asm_pinned_output_live_across: this example needs a \
constraint letter that BINDS A NAMED REGISTER, and only x86_64 declares one. \
`arm64.target.json`'s asmConstraints are r/w/m/i — all class- or form-bound — \
so no aarch64 asm statement can populate the per-instruction OUTPUT carrier \
this example exists to pin, and an arm here would witness nothing. Add one \
when a target declares a register-bound letter, not before."
#endif

volatile int dss_seed = 1;

int main(void) {
    int lo;
    int hi;
    int s;

    /* Fourteen independent live ranges, each a separate volatile read. */
    int a0  = dss_seed + 0;
    int a1  = dss_seed + 1;
    int a2  = dss_seed + 2;
    int a3  = dss_seed + 3;
    int a4  = dss_seed + 4;
    int a5  = dss_seed + 5;
    int a6  = dss_seed + 6;
    int a7  = dss_seed + 7;
    int a8  = dss_seed + 8;
    int a9  = dss_seed + 9;
    int a10 = dss_seed + 10;
    int a11 = dss_seed + 11;
    int a12 = dss_seed + 12;
    int a13 = dss_seed + 13;

    /* `"=a"` pins %rax and `"=d"` pins %rdx. Neither appears in a clobber
     * list — GNU C does not permit an output register to be written there, so
     * the LOWERING is what owes the clobber declaration the allocator reads. */
    __asm__ __volatile__ ("movl $11, %0\n\tmovl $22, %1"
                          : "=a"(lo), "=d"(hi));

    if (lo != 11) return 1;   /* the first pinned output did not arrive  */
    if (hi != 22) return 2;   /* the second did not, or read the first's */

    /* 14*1 + (0+1+…+13) == 14 + 91 == 105. A value that lived in %rax or
     * %rdx across the block is gone, and this sum is not 105. */
    s = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7
      + a8 + a9 + a10 + a11 + a12 + a13;

    return (s == 105) ? 42 : 3;
}
