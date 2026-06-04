// D-CSUBSET-DEREF-LOADSTORE-SMOKE (cycle 10h, 2026-06-04):
// Smoke-pin Deref-as-lvalue codegen end-to-end (source → HIR
// AssignStmt(Deref, ...) → MIR Store → asm mov-to-memory →
// caller observes the updated value).
//
// Pairs with sibling `deref_load` (rvalue side). Without this
// row the strict-aliasing capstone (cycle 10i,
// D-OPT-ALIAS-ARC-CORPUS-CAPSTONE-STRICT) would conflate a
// Store-codegen regression with a CSE clobber-walk regression.
//
// Witness-value discipline (post-fold 10h, multi-reviewer
// convergent FOLD-NOW): the slot is `long`-typed and pre-set
// to `0 - 1` (= 0xFFFFFFFFFFFFFFFF). The reason is
// discrimination of width bugs:
//   * `int` slot pre-set to 0 + 32-bit-Store-of-42: bytes are
//     `2A 00 00 00 -- -- -- --`; subsequent 32-bit Load reads
//     0x2A → exit 42, silent pass on the wrong-width regression.
//   * `int` slot pre-set to 0 + 64-bit-Store-of-42 (the wrong
//     width regression): bytes are `2A 00 00 00 00 00 00 00`;
//     subsequent 32-bit Load reads 0x2A → still exit 42, still
//     silent pass.
//   * `long` slot pre-set to -1 (all-ones) + correct 64-bit
//     Store-of-42: bytes become `2A 00 00 00 00 00 00 00`;
//     I64 Load reads 42 → exit 42.
//   * `long` slot pre-set to -1 + buggy 32-bit Store-of-42:
//     bytes are `2A 00 00 00 FF FF FF FF`; I64 Load reads
//     0xFFFFFFFF0000002A → exit 0 (the `if (x == 42)` arm
//     evaluates false on the upper-32 mismatch).
//
// Pre-call seeding via `0 - 1` rather than `-1` because
// c-subset doesn't have negative-literal syntax.

long write_through(long* p) {
    *p = 42;
    return 0;
}

int main() {
    long x;
    int r;
    x = 0 - 1;
    write_through(&x);
    r = 0;
    if (x == 42) {
        r = 42;
    }
    return r;
}
