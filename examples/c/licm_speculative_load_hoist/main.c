// D-OPT6-LICM-SPECULATIVE-LOAD-HOIST runtime miscompile pin.
//
// The sibling of licm_trap_safe_hoist/ for the OTHER may-fault family. That
// one pins a division; this one pins a DEREFERENCE, which is the shape
// ordinary C writes constantly: a pointer guarded inside a loop.
//
// WITNESSED LIVE before the gate existed: this program, compiled with the
// SHIPPED release pipeline, exited 0xC0000005 (STATUS_ACCESS_VIOLATION) while
// the debug build exited cleanly. The release LIR showed `load` on the
// null pointer sitting in the loop PREHEADER instead of the guarded block.
//
// `*p` is dereferenced ONLY inside `if (q)`, and `q` is 0 for every
// iteration, so a correct compiler never executes the load. `p` is
// loop-invariant and NULL. LICM's Load-hoist admission proves the loaded
// VALUE is invariant (no aliasing Store in the loop) — which is true — and
// that is NOT a licence to execute the load: hoisting it into the preheader
// makes the dereference UNCONDITIONAL and faults where the source never
// dereferences. Correct LICM refuses, because the `if (q)` arm is not
// guaranteed to execute. crash-vs-clean-exit is maximally bisectable.
//
// Every input rides a `volatile` global, which is load-bearing three times
// over — with plain initialised globals or literals, ConstFold/Mem2Reg would
// see `q == 0` and delete the guarded arm outright, or see `p == 0`, and the
// pin would be VACUOUS (it would pass with the gate removed):
//   * `guard_src` keeps the guard opaque, so the `if` survives to LICM;
//   * `trip_src` keeps the trip count opaque, so the loop is not unrolled
//     or deleted;
//   * `ptr_src` keeps the pointer opaque, so the load survives as a real
//     runtime dereference of address 0.
//
// `iters` is the anti-vacuity witness in the OTHER direction: the return
// value is `iters + x` = 7, so the loop must really have executed 7 times.
// A pipeline that deleted the loop, or a guard that folded the wrong way,
// changes the exit code rather than passing quietly.
//
// The `release` arm loads release.pipeline.json ITSELF (shippedPipeline),
// so this is the exact composition users get from `--config=release`, not a
// hand-picked pass subset that might not represent it.

volatile int guard_src = 0;   /* opaque 0 — the guard is never taken   */
volatile int trip_src  = 7;   /* opaque 7 — the loop really runs       */
int *volatile ptr_src  = 0;   /* opaque NULL — dereferencing it faults */

int main(void) {
    int  q = guard_src;
    int  n = trip_src;
    int *p = ptr_src;
    int  iters = 0;
    int  x = 0;
    for (int i = 0; i < n; i++) {
        iters = iters + 1;
        if (q) { x = x + *p; }   /* never taken: q == 0 every iteration */
    }
    return iters + x;            /* 7 + 0 */
}
