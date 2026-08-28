/*
 * D-OPT-SIMPLIFYCFG-EMPTY-SELF-LOOP-REDIRECT-CYCLE — the RUN witness.
 *
 * THE DEFECT. SimplifyCfg jump-threads "trampoline" blocks: a block whose
 * ONLY instruction is an unconditional `Br`. When a CFG cycle is made
 * ENTIRELY of trampolines, every member is a candidate, so the redirect map
 * acquires B1->B2->...->Bk->B1 and the transitive walk never terminates. The
 * pass ABORTED the whole compiler — `resolveTransitive exceeded chain length`,
 * rc=127, NO binary — on all three shipped targets, at `--config=release`
 * only. This file is the end-to-end proof that legal C of that shape now
 * COMPILES and RUNS.
 *
 * WHY THE LOOPS ARE PRESENT BUT NEVER ENTERED. An infinite loop that is
 * actually entered can never exit, so it cannot be a runnable example. Each
 * guard below is `argc > <big>`, and the harness runs the binary with NO
 * arguments (argc == 1), so every guard is false at runtime. `argc` is a
 * genuine RUNTIME value, so ConstFold cannot decide the branch and the loops
 * survive into the MIR that SimplifyCfg processes — which is the whole point:
 * the pass must SEE the shape. A literal `if (0)` would be folded away and
 * this example would be vacuous.
 *
 * WHY FOUR SHAPES. The defect is a cycle in the redirect map, and cycle
 * LENGTH is unbounded. The four arms cover k=2 (both via a loop statement and
 * via plain gotos, proving it is not about the `for` spelling), k=3, and k=1:
 *
 *   for (;;) {}                      -> header:Br(body), body:Br(header)  k=2
 *   a1: goto a2; a2: goto a1;        -> the same k=2 map, no loop statement
 *   b1: goto b2; b2: goto b3; b3:..  -> k=3
 *   c1: goto c1;                     -> k=1, a literal Br-to-self
 *
 * The k=1 arm is the one that ALREADY worked before the fix (a bespoke
 * self-target check covered exactly that length and nothing else). It is kept
 * here as a regression pin on the deletion of that bespoke check.
 *
 * SEMANTICS THE FIX MUST PRESERVE. The loops are collapsed, NOT deleted: each
 * cycle keeps one block that branches to itself. Were the optimizer to drop
 * them and fall through, a program that DID enter one would wrongly terminate.
 *
 * RED-ON-DISABLE: revert `breakJumpThreadCycles` in
 * src/opt/passes/simplify_cfg.cpp (and restore the bespoke `tgt.v == b.v`
 * check) -> the `release` arm's compile aborts with rc=127 and emits no
 * binary, so the example cannot run at all. The BASELINE arm stays green
 * either way — the defect is release-only, which is exactly why it survived
 * so long, and why the `release` shippedPipeline arm is the load-bearing one.
 *
 * exitCode 42.
 */
int main(int argc, char **argv) {
    (void)argv;

    /* k=2 — the canonical empty-bodied infinite `for`. */
    if (argc > 1000) { for (;;) { } }

    /* k=2 again, with no loop statement anywhere: a two-label goto ring. */
    if (argc > 1001) { a1: goto a2; a2: goto a1; }

    /* k=3 — cycle length is unbounded, so a 2-cycle-only fix is not a fix. */
    if (argc > 1002) { b1: goto b2; b2: goto b3; b3: goto b1; }

    /* k=1 — a literal Br-to-self. Green before the fix; must STAY green. */
    if (argc > 1003) { c1: goto c1; }

    return 42;
}
