// Audit-residue sweep c1 — D-AUDIT-FUSED-CMP-WIDTH-PIN runtime witness.
//
// The highest-traffic compare shape in C: a runtime-NEGATIVE int
// feeding `if (x < 0)`. The ICmpSlt's ONLY consumer is the CondBr, so
// MIR->LIR takes the FUSED cmp+jcc arm (mir_to_lir lowerCondBr) — a
// SEPARATE cmp emit site from the setcc value path. The fused cmp's
// width must follow the ICmp OPERANDS' type (I32 -> 32 bits):
//
//   x = sub(3, 5) = -2, I32, built at RUNTIME: the operands flow
//   through TWO calls (main -> check -> sub) as function args, and the
//   baseline (Identity) pipeline has no inlining/const-fold — the
//   subtraction, the compare, and the branch are all live machine code.
//
//   width-32 fused cmp:  -2 < 0          -> true  -> exit 42  (correct)
//   width-64 regression: the I32 sub writes its 32-bit result with the
//     upper 32 bits ZERO (x86 no-REX.W ops auto-zero-extend; arm64
//     W-forms zero bits 63:32), so a 64-bit cmp reads
//     0x00000000FFFFFFFE > 0                -> false -> exit 7.
//
// Exit arithmetic: 42 vs 7 — delta 35, NOT congruent 0 mod 256, so
// POSIX WEXITSTATUS (low 8 bits) cannot alias the two outcomes.
//
// HONESTY (the division/shift_ops precedent): under an optimizing
// pipeline (release: inline + const-fold) the whole chain folds to a
// constant return — the BASELINE arm is the witness. arm64 note: the
// same target-blind site threads the width there too; the fused cmp
// emits the W-form `CMP Wn, Wm` (SUBS WZR base 0x6B00001F, FC3-c2).

int sub(int a, int b) {
    return a - b;
}

int check(int a, int b) {
    int x = sub(a, b);
    if (x < 0) {
        return 42;
    }
    return 7;
}

int main() {
    return check(3, 5);
}
