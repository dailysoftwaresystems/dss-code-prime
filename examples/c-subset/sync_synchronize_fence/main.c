/* D-CSUBSET-ATOMIC-FENCE + D-CSUBSET-SYNC-BUILTIN-BARRIER: the
 * __sync_synchronize runtime witness — sqlite mutex_unix.c's sole pre-feature
 * error was `S0001 got __sync_synchronize` (sqlite3MemoryBarrier's GCC path).
 *
 * `__sync_synchronize()` is an always-on lowered builtin (the _ReadWriteBarrier
 * precedent) that lowers to MirOpcode::AtomicFence (payload seq_cst=5 — every
 * GCC __sync builtin is a FULL barrier) and, at MIR→LIR, to the target's
 * STANDALONE seq_cst fence instruction via the atomic_fence_seqcst slot:
 * x86-64 MFENCE (0F AE F0, 3 bytes), arm64 DMB ISH (0xD5033BBF, 4 bytes) —
 * both MEASURED via clang+otool on __sync_synchronize (2026-07-30).
 *
 * THE PROPERTY THIS RUNNABLE ARM PROVES: unlike _ReadWriteBarrier (which emits
 * ZERO instructions), each call splices a REAL 3-/4-byte fence instruction into
 * the middle of main's body — so a passing run proves the CPU accepts the
 * emitted fence encoding in situ: the surrounding instruction stream decodes
 * past it, branch offsets and the frame layout survive the insertion, and the
 * fence executes without faulting on real silicon (and under qemu-aarch64).
 * The ORDERING contract itself is unobservable in a single-threaded exit code
 * and is therefore pinned at the LIR/encoder tier instead: exact-count slot
 * routing + the 0..4-payload fail-loud arm (test_mir_to_lir), the exact fence
 * bytes (test_asm_x86_variable / test_asm_arm64), and the opcodeClobbersMemory
 * region-walk clobber (test_mir_alias).
 *
 * The `release` arm additionally proves the fence SURVIVES the full optimizer
 * pipeline (hasSideEffects keeps it off DCE/CSE/LICM; opcodeClobbersMemory
 * keeps the g stores/load on their sides of it) — a dropped AtomicFence would
 * emit no fence instruction, which the LIR exact-count pin reds structurally
 * while THIS arm keeps the whole compile+link+run chain honest per target.
 *
 * ANTI-FOLD: `g` is a mutable static global (the intrin_barrier idiom) so the
 * store/fence/store/fence/load sequence stays live to the optimizer.
 *
 * RED-on-disable: remove the __sync_synchronize row (c-subset.lang.json) →
 * S0001 undeclared — the EXACT pre-feature sqlite mutex_unix.c failure; remove
 * a target's atomic_fence_seqcst opcode row → L_RequiredLirOpcodeMissing (the
 * fail-loud belt; never a silent no-op "fence").
 *
 * exit = 22 + 20 = 42.
 */

static int g;

int main(void) {
    g = 20;
    __sync_synchronize();   /* MFENCE / DMB ISH spliced between the stores */
    g = 22;
    __sync_synchronize();   /* and between the last store and the load     */
    return g + 20;          /* 22 + 20 = 42 */
}
