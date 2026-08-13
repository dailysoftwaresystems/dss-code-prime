/* inline-asm P1 (D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED): an operand-carrying
 * `__asm__` is PARSED end to end and refused at the SEMANTIC tier with exactly ONE
 * diagnostic, S_InlineAsmExtendedUnsupported (S0062).
 *
 * SUPERSEDES `inline_asm_operands.c`, deleted in this cycle. That file pinned the
 * OLD behaviour — the grammar stopped at the first `:`, so `__asm__("" : : )` was a
 * 9-diagnostic parse cascade (P_UnexpectedToken x6 + P_MissingRequiredChild x2 +
 * P_BuilderInvariant) and the parser never recovered. Its subject now emits ZERO
 * diagnostics (it is a valid empty barrier), so its golden could not survive.
 *
 * ⛔ ACCEPT-AND-IGNORE IS THE FORBIDDEN OUTCOME, which is what this file guards. The
 * operands ARE the contract: `"=a"(lo)` says this statement writes `eax` and `lo`
 * receives it. Parsing them and lowering to a bare CompilerBarrier would clobber a
 * register the allocator still believes live AND leave `lo`/`hi` untouched — a silent
 * miscompile with a clean build log. So the refusal must be pinned, not just the
 * parse.
 *
 * ★ THE TEMPLATE IS EMPTY ON PURPOSE. The real-world shape is sqlite's
 * `src/hwtime.h:43` `__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi))`, but a
 * non-empty template ALSO earns S_InlineAsmNonEmptyTemplate (S0057, pinned separately
 * by `inline_asm_nonempty.c`). Emptying the template removes that race, so this golden
 * pins the operand refusal ALONE — if the extended gate regressed, S0057 would not
 * mask it.
 * ★ `lo`/`hi` ARE DECLARED. An undeclared operand would add a real S_UndeclaredIdentifier
 * line and quietly turn this from a one-code golden into a two-code one.
 *
 * RED-on-disable: drop any presence flag from the extended gate -> this compiles CLEAN
 * to a barrier with both outputs dropped, and the golden goes empty (which the harness
 * itself refuses: a zero-diagnostic corpus file is an ADD_FAILURE). */
int main(void) {
    unsigned lo, hi;
    __asm__ __volatile__ ("" : "=a"(lo), "=d"(hi));
    return 0;
}
