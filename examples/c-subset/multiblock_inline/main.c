// OPT7 cycle-2 MULTI-BLOCK inline correctness pin
// (D-OPT7-MULTIBLOCK-SPLICE), runtime tier.
//
// `pick` is a MULTI-BLOCK LEAF callee with TWO distinct return paths:
// `if (x > 0) return 7;` (then-block) falls through to a trailing
// `return 9;` (the after-if block) — the proven-clean early-return
// shape (cf. examples/c-subset/early_return) that lowers WITHOUT an
// unreachable join. The `x > 0` comparison yields an ICmp (Bool) →
// CondBr; a raw `if (x)` would lower to an i32→i1 `trunc` the x86_64
// schema doesn't encode. It is a leaf (no Call / IntrinsicCall), so
// the §2.9 gate admits it and the cycle-2 machinery splices it:
//   * main's call-site block is SPLIT at each `pick(...)` call;
//   * pick's blocks are CLONED into main (fresh ids, Linear markers);
//   * each cloned `Return` becomes a `Br` to a fresh CONTINUATION block;
//   * a RETURN-MERGE PHI in the continuation joins (7, then-clone) and
//     (9, else-clone) — that Phi is the call's result.
//
// main computes `pick(1) + pick(0)` = 7 + 9 = 16. The `Inlining`
// optimized arm MUST equal the baseline (16).
//
// RED-on-disable: if the return-merge Phi mis-wires (e.g. always
// selects one return value, or swaps the incoming predecessors), the
// two calls no longer yield 7 and 9 → the sum differs from 16 → the
// examples-runner differential ASSERT (optimized arm exit == baseline
// exit == 16) fires. Verified by temporarily forcing the merge to a
// single return path (wrong result) then restoring.

int pick(int x) {
    if (x > 0) {
        return 7;
    }
    return 9;
}

int main() {
    return pick(1) + pick(0);
}
