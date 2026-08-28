// FC4 c2 — the indirect-callee REGISTER-PRESSURE witness (the runtime
// arm of the regalloc rule pinned by tests/lir/test_lir_regalloc.cpp
// PressuredIndirectCalleeExcludesArgRegs).
//
// THE HAZARD: an indirect call's callee register is consumed AT the
// call, so it does not "cross" it — every caller-saved register,
// including the cc's ARG registers, is otherwise eligible. The
// callconv pass then inserts the arg-passing moves BETWEEN the
// callee's definition and the call: a callee parked in an arg
// register is overwritten by its own call's arg setup, and the call
// jumps THROUGH AN ARGUMENT VALUE. This example drains the pools so
// the allocator's natural pick for the callee REACHES the arg
// registers (on SysV x86_64) — only the FC4 c2 exclusion rule keeps
// it off them; without the rule the program crashes or exits != 42
// (and the L_IndirectCalleeClobberedByArgSetup backstop catches it
// loudly at compile time).
//
// Pressure shape (tuned against the SysV pools; other ccs simply run
// correctly with less drain):
//   * x, n, the post-call re-read of fp, and c0..c2 hold SIX ranges
//     ACROSS the call -> the callee-saved pool is full, so the callee
//     cannot retreat into it;
//   * the call's single argument SUMS t0..t3: each local's alloca
//     ADDRESS stays live ACROSS the callee load (its last use is its
//     arg-expression load, emitted after the callee load) -> four
//     caller-saved registers are held at the callee's allocation,
//     draining the LIFO toward the arg-register tail.
//   * ONE argument by design: a single arg-passing move can never
//     trip the conservative in-order move-hazard detector
//     (L_MoveCycleUnsupported needs a later move reading an earlier
//     move's destination — parallel-copy resolution for multi-arg
//     permutations is anchored at D-ML7-2.3 and is NOT this
//     example's subject).
//
// Fold-resistance: fp is RUNTIME-SELECTED between alpha and beta
// (the selector value n comes through the self-recursive sel — SCC
// refuses inlining, so n is never a constant) -> no pipeline can
// devirtualize the call.
//
// Exit arithmetic (x=1, n=sel(5)=5, alpha selected since n<=100):
//   arg = (x+4)+(x+5)+(x+6)+(x+7) = 4x+22 = 26 -> s = alpha(26) = 26
//   c-sum = (x+1)+(x+2)+(x+3) = 3x+6 = 9
//   total = s + x + n + z + c-sum = 26 + 1 + 5 + 1 + 9 = 42.
// beta would add 100 -> 142 (!= 42 mod 256); a clobbered callee
// jumps to an argument value (a small integer) -> crash, not 42.
int alpha(int a) { return a; }
int beta(int a) { return a + 100; }

int sel(int k) {
    if (k) { return sel(k - 1) + 1; }
    return 0;
}

int work(int x, int n) {
    int (*fp)(int) = &alpha;
    if (n > 100) { fp = &beta; }
    int c0 = x + 1;
    int c1 = x + 2;
    int c2 = x + 3;
    int t0 = x + 4;
    int t1 = x + 5;
    int t2 = x + 6;
    int t3 = x + 7;
    int s = fp(t0 + t1 + t2 + t3);
    int z = 0;
    if (fp != 0) { z = 1; }
    return s + x + n + z + c0 + c1 + c2;
}

int main() { return work(1, sel(5)); }
