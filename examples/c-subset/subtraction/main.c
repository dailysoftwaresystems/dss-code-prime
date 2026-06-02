// D-CSUBSET-BINOP-RIGHT-CLOBBER closure pin (NON-commutative arm).
//
// Subtraction is NOT commutative — `op0 - op1 != op1 - op0`. The
// regalloc-tier fix (excluding op[1..N]'s physical registers from
// the result allocation for any `requires2Address` instruction)
// must prevent the silent miscompile here too, NOT just for the
// commutative ops. Pre-fix, this binary would have silently
// returned 0 (= r - r when ops[1] aliases result) for `50 - 8`.
//
// User insight (2026-06-02): "sub is in c-subset right now, it's
// requires2Address, and it's non-commutative. Any `return 50 - 8;`
// that hits the same regalloc reuse pattern the 42 + 0 example hit
// will fail the same way." Adding this example PROVES the regalloc
// fix covers both arms uniformly — if subtraction had remained
// silent-miscompiled, this example would fail with exit code != 42.
//
// 50 - 8 = 42. Always-run regression pin.
int main() {
    return 50 - 8;
}
