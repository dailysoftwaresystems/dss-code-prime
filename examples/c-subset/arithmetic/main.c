// D-CSUBSET-BINOP-RIGHT-CLOBBER closure pin (commutative arm).
//
// Multiplication is COMMUTATIVE — pre-fix, this exercised the
// silent miscompile: the regalloc would assign the result to v2's
// just-freed register, then legalize-tier emitted `mov result,
// op0; mul result, [result, result]` = `op0 * op0` instead of
// `op0 * op1`. For `6 * 7` that gave 6 * 6 = 36 (observed).
//
// Post-fix (regalloc-tier exclusion of op[1..N] registers from
// the result allocation): the conflict is prevented by
// construction. legalize-tier emits `mov result, op0; mul
// result, op1`. Exit code MUST be 42.
//
// Always-run regression pin — a regression in any tier (regalloc
// candidate-exclusion, legalize semantics, encoder, walker)
// flips the exit code and fails the harness immediately.
int main() {
    return 6 * 7;
}
