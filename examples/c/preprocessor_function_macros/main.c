// FC13 cycle 2 -- the C-preprocessor FUNCTION-like macro runtime witness.
// Exercises, end to end through every target leg, the cycle-2 substrate:
//   * a `#define ADD(a,b)` / `SQUARE(x)` / `MAX(a,b)` whose PARAMETERS are
//     substituted by call-site ARGUMENTS, then RESCANNED;
//   * NESTED + COMPOSED invocations (ADD(SQUARE(a), MAX(b,c))) -- the inner
//     calls are arguments, fully pre-expanded before the outer substitution;
//   * an argument that is itself an OBJECT macro (BIAS), proving the
//     argument-pre-expansion order (C 6.10.3.1).
//
// Fold-resistance: every macro operand is a FUNCTION ARGUMENT seeded through
// `ident` (an opaque pass-through the optimizer cannot fold to a constant at
// -O0), so the baseline arm keeps live runtime arithmetic. The PRIMARY witness
// is still compile-time: a dropped/garbled macro expansion leaves an undefined
// identifier (or wrong arity) and the program never links.
//
//   a=5, b=8, c=9, BIAS=0
//   SQUARE(a)        = (a)*(a)        = 25
//   MAX(b,c)         = (b)>(c)?(b):(c) = 9
//   ADD(25, 9)       = (25)+(9)       = 34
//   ADD(34, ident(8)) with the +BIAS tail = 34 + 8 + 0 = 42  -> exit 42
#define ADD(a, b)    ((a) + (b))
#define SQUARE(x)    ((x) * (x))
#define MAX(a, b)    ((a) > (b) ? (a) : (b))
#define BIAS         0

int ident(int x) {
    return x;
}

int compute(int a, int b, int c) {
    return ADD(ADD(SQUARE(a), MAX(b, c)), ident(b)) + BIAS;
}

int main(void) {
    return compute(ident(5), ident(8), ident(9));
}
