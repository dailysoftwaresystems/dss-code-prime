/* D-LIR-ASM-TEMPLATE-EMITTING-NO-INSTRUCTION-ABORTS-WITH-NO-DIAGNOSTIC — an
 * inline-asm template that PARSES but lowers to ZERO instructions.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. Every template below compiled to a process
 * ABORT before the fix — not a diagnostic, an abort:
 *
 *   int main(void) { __asm__ ("   "); return 42; }
 *   → rc=127, `dss::Lir fatal: LirBuilder::lastInst: no instruction has been
 *     appended`
 *
 * No diagnostic code, no source position, nothing naming the construct. ✔The
 * five shapes below were MEASURED aborting at the CLI on `pe64-x86_64` and
 * `elf64-x86_64`, while gcc 13.3.0 compiles and runs every one of them
 * returning 42 — one working reference makes the behaviour REQUIRED (bar
 * §A.3b).
 *
 * ★★★ THE DEFECT WAS SHADOW STATE, NOT ASSEMBLY. `mir_to_lir` needed "the id
 * `addInst` will mint next", which has to distinguish the empty module.
 * `LirBuilder` exposed no count, so the lowering kept a `bool` MIRRORING the
 * arena. The assembly engine emits straight into the builder, bypassing the
 * site that set the mirror, so the asm path re-derived it FROM THE SHAPE OF
 * THE INPUT — "did the template carry a non-blank line?" — on the argument
 * that a line with content always lowers to at least one instruction. That
 * argument ENUMERATED its exceptions (a directive, a label, both already
 * refusals) and missed the ones below, so the mirror was set true over an
 * EMPTY arena and `lastInst()` aborted. The fix is `LirBuilder::hasAnyInst()`
 * — the count whose absence forced the mirror — after which the lowering
 * PROBES the arena and the mirror is deleted rather than repaired.
 *
 * ★★ WHY THE FIRST STATEMENT IS THE LOAD-BEARING ONE, AND WHY THAT IS NOT A
 * WEAKNESS OF THE TEST. `lastInst()` aborts only on an arena that is EMPTY, so
 * ONLY a zero-instruction template with nothing emitted before it in the whole
 * module reaches the abort. ✔MEASURED: the identical template placed after any
 * other statement compiles rc=0 both before and after the fix. That makes
 * statement 1 the red-on-disable witness and statements 2–5 coverage of the
 * shapes that share its path — deliberately stated here so a later reader does
 * not "simplify" the ordering and silently disarm the example.
 *
 * ⚠ `#`-style comments are NOT in this file on purpose. `__asm__ ("# text")`
 * is the same lowering path, but `#` is an x86 gas spelling: ✔MEASURED, the
 * arm64 dialect refuses it fail-loud with `error[P0009]`, which is CORRECT.
 * A portable example must not carry a dialect-private spelling, so the shapes
 * below are the ones every shipped dialect reads identically.
 *
 * ⓘ WHAT THE EXIT CODE DOES AND DOES NOT WITNESS. It witnesses that each
 * template COMPILES, LINKS and RUNS — which is precisely what the abort denied.
 * It does NOT witness the memory clobber in statement 6: both loads read the
 * same unchanging seed, so a lost barrier would still return 42. That property
 * was measured separately, by disassembly (two loads of the global, no CSE,
 * and the emitted image byte-identical to the empty-template control). An
 * execution witness for it would need the value to change between the loads,
 * which needs a second thread — a different example.
 */

volatile int dss_seed = 42;

int main(void) {
    /* 1 — FIRST in the module: the arena is empty here. This is the statement
     *     that aborted, and the one a revert reds on. */
    __asm__ ("/* this template emits no instruction */");

    /* 2 — whitespace only */
    __asm__ ("   ");

    /* 3 — a bare newline */
    __asm__ ("\n");

    /* 4 — newline, spaces, newline: several lines, still no instruction */
    __asm__ ("\n   \n");

    /* 5 — the byte-EMPTY template. The control that never aborted, and the
     *     reason the boundary is exact: an empty template never reaches this
     *     lowering at all — it becomes a `CompilerBarrier`, which lowers to
     *     nothing. Everything above had to travel through `expandInlineAsm`. */
    __asm__ ("");

    /* 6 — a zero-instruction template carrying a MEMORY CLOBBER still has to
     *     be a barrier. See the note above on what the exit code proves. */
    int a = dss_seed;
    __asm__ ("   " : : : "memory");
    int b = dss_seed;

    return (a + b) / 2;
}
