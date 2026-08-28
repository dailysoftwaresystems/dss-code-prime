// FC15 paste residuals -- the end-to-end runtime witness that COMPLETES `##`:
//
//   (1) OBJECT-like `##` (D-PP-PASTE-OBJECT-LIKE, C 6.10.3.3): `#define MK3 add##3`
//       expands `MK3` to the single token `add3` (a function name). The call
//       `MK3(10, 20, 12)` only links if the object-like paste product reaches the
//       parser AS `add3` -- not as `add`/`3`/`##`. add3 returns the sum -> 42.
//   (2) PLACEMARKER (D-PP-PASTE-PLACEMARKER, C 6.10.3.3p2): `CAT(a,b) a##b` invoked
//       as `CAT(r1, )` has an EMPTY right operand -> `r1 ## <placemarker>` -> `r1`.
//       If the placemarker were not modeled, `r1 ##` is a dangling `##` -> the file
//       FAILS TO COMPILE (P_PreprocessorPaste). So a clean compile + r2 == r1 is the
//       witness.
//   (3) GNU COMMA-ELISION (D-PP-VARIADIC-GNU-COMMA-ELISION): `LOG(fmt, ...)` uses
//       `sink(fmt, ## __VA_ARGS__)`. `LOG(r2)` has an EMPTY variadic part, so the
//       comma before `## __VA_ARGS__` is DROPPED -> `sink(r2)` (a one-arg call). If
//       the comma were NOT elided, `sink(r2, )` calls the 1-arg `sink` with two
//       arguments -> a compile error. So a clean compile + r3 == r2 is the witness.
//
// Fold-resistance: the paste/elision products feed REAL non-inlined function CALLs
// (add3, sink) -- not const-folded to an immediate at the unoptimized exit -- and
// the optimizedPipelines `release` arm runs the SHIPPED pipeline over the same
// source, so the optimizer x feature composition is exercised at runtime.
//
//   add3(10,20,12) = 42 ; CAT(r1,) = r1 = 42 ; LOG(r2) = sink(42) = 42 -> exit 42.

#define MK3            add ## 3
#define CAT(a, b)      a ## b
#define LOG(fmt, ...)  sink(fmt, ## __VA_ARGS__)

int add3(int a, int b, int c) {
    return a + b + c;
}

int sink(int v) {
    return v;
}

int main(void) {
    // (1) object-like ## -> add3; runtime call.
    int r1 = MK3(10, 20, 12);   // 42

    // (2) placemarker: empty right operand -> r1.
    int r2 = CAT(r1, );         // -> r1 -> 42

    // (3) GNU comma-elision: empty __VA_ARGS__ drops the comma -> sink(r2).
    int r3 = LOG(r2);           // -> sink(42) -> 42

    if (r1 != 42) return 1;
    if (r2 != 42) return 2;
    return r3;                  // 42
}
