// Regression corpus for D-HIR-LOOP-BODY-ONLY-RETURN-DOUBLE-ATTACH:
// `main`'s body is a single provably-infinite loop whose ONLY exit is
// a `return`. The body does not structurally terminate (the verifier
// treats `while(1)` as non-terminating), so HIR lowering appends an
// implicit `return 0` (C99 §5.1.2.2.3). The append used to re-wrap the
// body's already-parented children and trip the HirBuilder double-
// attach guard (std::abort); the immutability-safe nest fixes it.
// The dead `return 0` is pruned by MIR's unreachable pass, so the
// observable exit is the loop's `return 5`.
int main() {
    while (1) {
        return 5;
    }
}
