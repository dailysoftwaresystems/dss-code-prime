/* FC17.9(i) (D-CSUBSET-INLINE-ASM): the empty-template `__asm__` optimizer-barrier
 * witness.
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
 * Two stores to a global straddle two barriers, then a load. A single-threaded
 * program cannot OBSERVE compiler reordering, so this returns 42 either way — it
 * witnesses the COMPILE+RUN chain (the `__asm__` keyword lexes, the asmStmt rule
 * parses, the empty template is accepted at semantic, the InlineAsm HIR leaf lowers
 * to CompilerBarrier, the binary runs), including under the `release` optimizer arm
 * (the barrier must survive DCE/CSE/LICM). The ordering CONTRACT (a load is not
 * hoisted across the fence) is pinned structurally in the MIR tests, where it is
 * observable; the asm→barrier LINK is pinned in test_mir_lowering_c_subset.
 *
 * RED-on-disable: remove the `__asm__` keyword / asmStmt rule (c-subset.lang.json)
 * -> P0001 (`__asm__` lexes as an identifier again); or a non-empty template here ->
 * S0057 (S_InlineAsmNonEmptyTemplate). The barrier being DROPPED (asmStmt mapped to
 * Skip) is invisible to this exit-code test — that is pinned at the MIR tier instead.
 */

static int g;

int main(void) {
    g = 20;
    __asm__ volatile ("");
    g = 22;
    __asm__ volatile ("");
    return g + 20;   /* 22 + 20 = 42 */
}
