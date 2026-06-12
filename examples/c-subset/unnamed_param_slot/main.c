// FC4 c1 (review-fold NIT #1) — the mixed named/unnamed parameter
// ABI-slot pin. C23 allows unnamed parameters in definitions; the
// FnSig keeps a slot for every parameter, named or not, so the
// NAMED `b` below must read arg-register slot 1 (x86_64 SysV rdx /
// ms_x64 rdx is slot-dependent; arm64 w1) — NOT slot 0.
//
// Discriminator: a regression that drops the unnamed slot (so `b`
// binds slot 0) returns pick's FIRST argument: 7 + 9 = 16 instead
// of 33 + 9 = 42. Delta 26 ≢ 0 mod 256 — WEXITSTATUS-safe on every
// leg. Arguments arrive as runtime literals at the call site of a
// non-inlined-by-baseline function, so no earlier pass can fold the
// selection away in the baseline arm.
int pick(int, int b) {
    return b;                       // slot 1 — the witness
}

int main() {
    return pick(7, 33) + 9;        // 33 + 9 = 42 (slot-shift -> 16)
}
