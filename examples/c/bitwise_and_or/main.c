// Audit-residue sweep c1 — D-AUDIT-BITWISE-UNWALL-WITNESS runtime corpus.
//
// FC3.5 sweep-c2 added the x86 `and`/`or` encodings (21 /r and 09 /r,
// widths 64+32) for the composed-FCmp materialization — which silently
// UN-WALLED source-level `a & b` / `a | b` end-to-end on BOTH targets
// (arm64's AND/ORR X-form encodings pre-existed). Until this example
// they compiled AND ran with zero corpus witness. `^` and `~` stay
// walled: x86 declares xor/not WITHOUT encodings (assembler
// A_NoEncodingDeclared); arm64 encodes EOR but declares no `not`
// mnemonic (lowering L_RequiredLirOpcodeMissing). No consumer -> no
// speculative encoding.
//
// Runtime operands (fn args through two calls, main -> check ->
// band/bor; the baseline Identity pipeline has no inlining/const-fold,
// so the bitwise ops are live machine instructions):
//
//   a = 76 = 0x4C = 0b0100'1100,  b = 42 = 0x2A = 0b0010'1010
//
//   a & b = 0b0000'1000 =   8   <- asserted (first equality)
//   a | b = 0b0110'1110 = 110   <- asserted (second equality)
//   a ^ b = 0b0110'0110 = 102   (xor-misroute candidate)
//   a + b =               118   (add-misroute candidate)
//
// All four candidate values are pairwise DISTINCT and < 256 (verified:
// 8 != 110 != 102 != 118), so ANY single misroute (&->|, &->+, &->^,
// |->&, |->+, |->^) flips at least one equality below -> exit 7. The
// discrimination happens IN-REGISTER (a full-width compare feeding the
// branch), never via exit-code truncation; the exits themselves
// (42 vs 7, delta 35) are NOT congruent mod 256, so POSIX WEXITSTATUS
// (low 8 bits) cannot alias the outcomes either.
//
// HONESTY (the division/shift_ops precedent): under an optimizing
// pipeline (release: inline + const-fold) the chain folds to a
// constant return — the BASELINE arm is the witness.

int band(int a, int b) {
    return a & b;
}

int bor(int a, int b) {
    return a | b;
}

int check(int a, int b) {
    int x = band(a, b);
    int y = bor(a, b);
    if (x == 8) {
        if (y == 110) {
            return 42;
        }
    }
    return 7;
}

int main() {
    return check(76, 42);
}
