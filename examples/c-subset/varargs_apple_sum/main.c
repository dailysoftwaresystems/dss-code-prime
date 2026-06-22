// FC12c (D-FC12C-APPLE-ARM64-VARIADIC-CALLEE): the Apple arm64 (Mach-O) variadic
// callee, realizing the variadic half of D-FF3-APPLE-ARM64-ABI-DIVERGENCE. Apple
// passes ALL variadic args on the STACK (vs AAPCS64's register-then-stack), so
// `va_list` is a PLAIN POINTER (the HomogeneousPointer strategy, like Win64) that
// linearly bumps over the contiguous overflow area — no register-save-area, no dual
// cursor. `sum` takes one fixed int `n` then `n` variadic ints; `main` calls
// sum(4, 10, 20, 30, 40) -> 100 -> exit 100.
//
// Exercises the Apple-specific seams:
//   * The CALLER forces every vararg (operand past the fixed `n`) onto the stack
//     overflow area regardless of free arg registers (variadicArgsAlwaysStack) — the
//     SAME call under AAPCS64 keeps the varargs in x1..x4.
//   * va_start anchors `ap` at the OVERFLOW base (variadicUsesOverflowBase) — the first
//     stacked vararg — not a home area (Apple has none; the named `n` stays in x0).
//   * The prologue spills NOTHING (no home, no register-save-area).
//   * va_arg(ap, int) is the linear pointer bump (the HomogeneousPointer walk).
//
// COMPILES on every host (a structural witness of the whole c-subset -> Mach-O-arm64
// cross-compilation + the always-stack placement); RUNS only on the macos-latest CI
// leg (no qemu for Mach-O). SkippedCrossHost on a non-macOS host — expected, NOT a
// failure.

int sum(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int t = 0;
    for (int i = 0; i < n; i = i + 1) {
        t = t + va_arg(ap, int);
    }
    va_end(ap);
    return t;
}

int main(void) {
    return sum(4, 10, 20, 30, 40);
}
