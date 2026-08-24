// FC5 (audit MUST-FIX 3) — a `goto` that ESCAPES a provably-infinite loop. The
// `while (1)` has NO `break`, so the frontend wraps it `Block{loop, Unreachable}`
// (the infinite-loop-termination treatment); the goto's `Br` to `out` is the
// loop's ONLY real exit. This pins that the interaction is runtime-correct: the
// wrap's synthetic `Unreachable` lands on a no-predecessor block that the
// mandatory MIR unreachable-prune drops, while the goto edge keeps `out` live —
// the program EXITS 42, never hangs and never trips the verifier. Fold-resistant:
// runtime bound.
int run(int n) {
    int acc = 0;
    while (1) {
        if (acc >= n) goto out;     // the ONLY exit — a frame-escaping goto
        acc = acc + 6;
    }
out:
    return acc;                     // acc: 0,6,...,42
}

int main() {
    return run(42);                 // exit 42
}
