/* FC17.9(i) (D-CSUBSET-INLINE-ASM) + inline-asm P1
 * (D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED): the `__asm__` optimizer-barrier
 * witness — the empty template AND every empty-SECTION spelling of it.
 *
 * `__asm__ volatile ("")` is the GNU inline-asm statement with an empty template. It
 * emits NO machine instruction; its whole effect is a COMPILE-TIME reordering +
 * full-memory fence (it lowers to a zero-operand, side-effecting MIR op —
 * MirOpcode::CompilerBarrier, the same op _ReadWriteBarrier uses — that forbids the
 * optimizer from moving memory accesses across it). Unlike <intrin.h>'s
 * _ReadWriteBarrier, `__asm__` is PURE SYNTAX and fully target-agnostic: the barrier
 * lowers to nothing on every target, so this example runs on all four legs (no gated
 * header, no per-target code).
 *
 * ★★ WHY THE `:`-SECTION FORMS ARE HERE, AND WHY THIS IS NOT A DIAGNOSTIC-ONLY
 * FEATURE. P1 widened the grammar to parse GNU's full extended form so an
 * operand-carrying asm could be REFUSED precisely (one S0062 instead of a 53-message
 * non-recovering parse cascade). But the same widening ADMITTED a family of forms
 * that were previously parse errors and are now perfectly good barriers: a section
 * that is OPENED AND EMPTY binds nothing, so there is nothing to miscompile.
 * ✔MEASURED on gcc 13.3.0 (-O2, -std=gnu17 / -std=gnu2x / -std=c2x): every form
 * below compiles and the program EXECUTES, returning 42. That is runtime-observable
 * behaviour newly admitted by DSS, so it needs a RUNNABLE witness — a diagnostic
 * golden could only have shown the absence of an error, never that the barrier
 * survives lowering, register allocation, the optimizer and the loader.
 *
 * ★ ONE FORM PER GRAMMAR BRANCH CHAIN, NOT PER SPELLING. GNU's four optional
 * sections are modelled as a CHAIN of boundary rules, each with a plain arm and a
 * FUSED arm (`::` arrives from the tokenizer as ONE token — maximal munch, because
 * any C23 host must also spell the attribute-namespace separator). Every clean-
 * reachable path through that chain appears below exactly once:
 *
 *     (no tail)      asmStmt with the `{optional asmOutputsTail}` absent
 *     ("" : )        asmOutputsTail, plain arm, nothing after it
 *     ("" : : )      -> asmInputsTail, plain arm
 *     ("" :: )       -> asmInputsTailFused          (fused outputs->inputs)
 *     ("" : : : )    -> asmInputsTail -> asmClobbersTail, all plain
 *     ("" ::: )      -> asmInputsTailFused -> asmClobbersTail
 *     ("" : ::)      -> asmInputsTail's FUSED arm -> asmClobbersTailFused
 *
 * The two LABEL-section rules (asmLabelsTail / asmLabelsTailFused) are deliberately
 * absent: a fourth section is ill-formed without `goto` (S0063) and unsupported with
 * it (S0062), so neither is clean-reachable and both are pinned in the diagnostics
 * corpus instead. Spellings that differ only in TOKEN TEXT are also absent — DSS
 * aliases `__volatile__` onto `volatile`'s kind, so it exercises no new grammar path
 * and is covered by the conformance probe a_asm_volatile_gnu.
 *
 * ★ `__asm__ inline ("")` is the third qualifier of the order-free qualifier run
 * (`{repeat {alt volatile|inline|goto}}`). `volatile` and `inline` are both
 * SEMANTICALLY INERT for a no-output asm; `goto` is not, and is refused. A REPEATED
 * qualifier (`volatile __volatile__`) is rejected — S0064, pinned in the corpus.
 *
 * Two stores to a global straddle the barrier run, then a load. A single-threaded
 * program cannot OBSERVE compiler reordering, so this returns 42 either way — it
 * witnesses the COMPILE+RUN chain (the `__asm__` keyword lexes, asmStmt and the whole
 * boundary chain parse, the empty template and the empty sections are accepted at
 * semantic, every InlineAsm HIR leaf lowers to CompilerBarrier, the binary runs),
 * including under the `release` optimizer arm (the barriers must survive
 * DCE/CSE/LICM). The ordering CONTRACT (a load is not hoisted across the fence) is
 * pinned structurally in the MIR tests, where it is observable; the asm->barrier LINK
 * is pinned in test_mir_lowering_c_subset.
 *
 * ⚠ WHAT THIS EXIT-CODE TEST CANNOT SEE, stated plainly and kept from the original
 * form: a DROPPED barrier. If asmStmt were mapped to Skip — for any of the forms
 * below, including the newly-admitted empty-section ones — this example would still
 * return 42 and pass. Barrier PRESENCE is pinned at the MIR tier by a separate lane.
 * What this file does witness is that admitting these forms did not break the
 * compile+run chain on any leg or optimizer arm.
 *
 * RED-on-disable: remove the `__asm__` keyword / asmStmt rule (asm.lang.json) -> P0001
 * (`__asm__` lexes as an identifier again); remove any boundary rule from the tail
 * chain -> the corresponding line below becomes a parse error; a non-empty template
 * here -> S0057 (S_InlineAsmNonEmptyTemplate); putting a real OPERAND in any section
 * below -> S0062 (S_InlineAsmExtendedUnsupported). The barrier being DROPPED
 * (asmStmt mapped to Skip) is invisible to this exit-code test — that is pinned at
 * the MIR tier instead.
 */

static int g;

int main(void) {
    g = 20;
    __asm__ volatile ("");   /* no section tail at all */
    __asm__ ("" : );         /* outputs boundary, section empty */
    __asm__ ("" : : );       /* + inputs boundary */
    __asm__ ("" :: );        /* fused outputs->inputs */
    g = 22;
    __asm__ ("" : : : );     /* + clobbers boundary, all plain */
    __asm__ ("" ::: );       /* fused outputs->inputs, then plain clobbers */
    __asm__ ("" : ::);       /* plain outputs, then fused inputs->clobbers */
    __asm__ inline ("");     /* the `inline` qualifier */
    __asm__ volatile ("");
    return g + 20;   /* 22 + 20 = 42 */
}
