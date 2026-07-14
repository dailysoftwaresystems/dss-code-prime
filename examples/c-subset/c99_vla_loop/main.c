// D-CSUBSET-VLA C5: BLOCK-SCOPE TEARDOWN. A variable-length array descends the
// stack (`sub sp, <aligned n*4>`); C5 makes that lifetime BLOCK-scoped by restoring
// SP on every NON-return exit of the declaring scope. WITHOUT C5 the `sub sp`
// accumulates once per loop iteration and SP marches off the end of the stack —
// STATUS_STACK_OVERFLOW (0xC00000FD). WITH C5 each iteration reclaims its VLA, so
// the loops are bounded and run to completion.
//
// This example is a RED-ON-DISABLE CRASH witness: phase 1 alone allocates a VLA
// 400000 times in a loop body (~12.8 MB of leaked stack without teardown). It also
// exercises EVERY non-return exit edge the teardown must cover — natural back-edge,
// `continue`, `break`, a nested `{ }` block, and `goto` out of a VLA scope — and
// checks a deterministic running total, so a MIS-PLACED restore (freeing the array
// before its last read) also flips the exit code, not only a crash.
//
//   phase 1 (back-edge):  400000 iters * (a[0]=1, a[n-1]=1)          => += 2 each   = 800000
//   phase 2 (continue):   100000 iters * (b[0]=1) then `continue;`   => += 1 each   = 100000
//   phase 3 (break):      c[0]=5 once, then `break;`                 => += 5        =      5
//   phase 4 (nested {}):  100000 iters * (inner { d[0]=3 })          => += 3 each   = 300000
//   phase 6 (for-init):   400000 iters * (inner f[n], read at k=1,2) => += 14 each  = 5600000
//   phase 5 (goto out):   e[0]=7 once, then `goto done;`             => += 7        =      7
//   total = 800000 + 100000 + 5 + 300000 + 5600000 + 7 = 6800012  => return 42
int main(void) {
    volatile int n = 8;   // runtime VLA length; volatile defeats constant-folding
    int i;
    long total;
    total = 0;

    // (1) THE CRASH SHAPE: a VLA in a large loop body. Restored on the back-edge.
    for (i = 0; i < 400000; i = i + 1) {
        int a[n];
        a[0] = 1;
        a[n - 1] = 1;
        total = total + a[0] + a[n - 1];
    }

    // (2) `continue`: a VLA, then a continue that reclaims it on the continue edge.
    for (i = 0; i < 100000; i = i + 1) {
        int b[n];
        b[0] = 1;
        total = total + b[0];
        if (i >= 0) { continue; }
        total = total + 999;   // unreachable — proves the continue actually fires
    }

    // (3) `break`: a VLA reclaimed on the break edge (exits the loop).
    for (i = 0; i < 100000; i = i + 1) {
        int c[n];
        c[0] = 5;
        total = total + c[0];
        if (i == 0) { break; }
    }

    // (4) nested `{ }` block: a VLA in an inner scope, reclaimed at the inner
    // block's fall-through exit each iteration (the loop body itself has no VLA).
    for (i = 0; i < 100000; i = i + 1) {
        {
            int d[n];
            d[0] = 3;
            total = total + d[0];
        }
    }

    // (6) for-INIT VLA in an outer loop — the C5 for-SCOPE teardown witness. The inner
    // `f[n]` is declared ONCE per outer iteration (at the inner loop's entry) and
    // PERSISTS across the inner loop's iterations (written at k==0, read at k>0), then
    // freed ONLY when the inner loop EXITS (not the back-edge). Two failure modes are
    // caught: freeing it on the back-edge = a use-after-free corrupting the k>0 reads
    // (wrong total); NOT freeing it = a per-outer-iteration leak that overflows.
    for (i = 0; i < 400000; i = i + 1) {
        int k;
        k = 0;
        for (int f[n]; k < 3; k = k + 1) {
            if (k == 0) { f[0] = 7; }        // write once; must survive the back-edge
            else { total = total + f[0]; }   // read at k=1,2 => += 14 per outer iter
        }
    }

    // (5) `goto` OUT of a VLA scope: reclaimed on the goto edge (legal outward jump).
    {
        int e[n];
        e[0] = 7;
        total = total + e[0];
        goto done;
    }
done:
    if (total == 6800012) { return 42; }
    return 7;
}
