// FC13 cycle 3 -- the C-preprocessor VARIADIC (`__VA_ARGS__`) macro runtime
// witness. Exercises, end to end through every target leg, the cycle-3
// substrate (plain `__VA_ARGS__`, NO `#`/`##`, NO GNU comma-elision):
//   * `CALL(fn, ...)` -- a NAMED param (`fn`) PLUS a variadic catch-all: the
//     trailing arguments ride `__VA_ARGS__` (their separating commas preserved)
//     into a fixed-arity function call;
//   * `FWD(...)`      -- a ZERO-NAMED variadic macro (every argument is a
//     trailing arg);
//   * `HEAD(first, ...)` invoked with an EMPTY variadic part (C23 -- `__VA_ARGS__`
//     substitutes to nothing), proving the empty-variadic case compiles + runs.
//
// Fold-resistance: every macro operand reaches the callees as a FUNCTION
// ARGUMENT seeded through `ident` (an opaque pass-through the optimizer cannot
// fold to a constant at -O0), so the baseline arm keeps live runtime arithmetic
// over the variadic-forwarded values. The PRIMARY witness is still compile-time:
// a dropped/garbled `__VA_ARGS__` expansion leaves a wrong arity (or an
// undefined identifier) and the program never links.
//
//   add3(20, 15, 7)            = 42                      (via CALL + __VA_ARGS__)
//   add3 via FWD(20,15,7)      = 42                      (zero-named variadic)
//   HEAD(42)                   = 42                      (empty __VA_ARGS__)
//   result = add3(...) ; cross-checked == 42             -> exit 42
#define CALL(fn, ...) fn(__VA_ARGS__)
#define FWD(...)      add3(__VA_ARGS__)
#define HEAD(first, ...) first

int ident(int x) {
    return x;
}

int add3(int a, int b, int c) {
    return a + b + c;
}

int main(void) {
    // (1) NAMED + variadic: CALL(add3, ...) forwards the three runtime-seeded
    // trailing args (commas preserved) -> add3(20, 15, 7) = 42.
    int viaCall = CALL(add3, ident(20), ident(15), ident(7));

    // (2) ZERO-NAMED variadic: every arg is a trailing arg -> add3(20, 15, 7).
    int viaFwd = FWD(ident(20), ident(15), ident(7));

    // (3) EMPTY __VA_ARGS__ (C23): HEAD keeps only its first (named) arg; the
    // empty variadic part substitutes to nothing.
    int viaHead = HEAD(ident(viaCall));

    // All three are 42; pick the named+variadic path as the exit code and guard
    // that the other two agree (a garbled variadic forward would diverge).
    if (viaFwd != viaCall) {
        return 1;
    }
    if (viaHead != viaCall) {
        return 2;
    }
    return viaCall;
}
