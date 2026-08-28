// Truthiness (V2-4.X, 2026-06-11): a BARE int condition — the C99
// 6.8.4.1p2 "compares unequal to 0" semantic — runtime-witnessed.
//
// The condition VALUE is 2, routed through a function arg so the
// baseline pipeline cannot const-fold the test away: a real compare
// instruction evaluates `v != 0` at runtime.
//
//   truthiness (correct):  v = 2 → `2 != 0` is TRUE  → exit 42
//   low-bit truncation     v = 2 → Trunc(2 → i1) = 0 → exit  7
//     (the old Cast-to-Bool lowering — exit-DIVERGENT by construction)
//
// The runner asserts exit code 42 exactly on all four target arms.
int test(int v) {
    if (v) return 42;
    return 7;
}

int main() {
    return test(2);
}
