// FC13 cycle 4 -- the C-preprocessor PRECISE per-token HIDE-SET (Prosser,
// C 6.10.3.4) runtime witness. Exercises, end to end through every target leg,
// the cross-stream-boundary re-pairing the cycle-2/3 recursion-scoped blue-paint
// could NOT do (it froze a function-like name whose `(` lived in the parent
// stream). With the precise hide set a macro name and a `(` that become adjacent
// only ACROSS the replacement/parent boundary RE-PAIR and expand:
//
//   * OBJECT macro NAMING a function-like macro, then invoked:
//       #define NAME SQ ; NAME(a)  ->  SQ  (hide {NAME}) re-pairs with the
//       parent's `(a)`  ->  ((a)*(a)).  Under the old paint NAME(a) froze to a
//       bare `SQ` (a downstream parser error), never expanding.
//   * A function-like macro that RETURNS a function-like name, then invoked:
//       #define APPLY(f) f ; APPLY(INC)(b)  ->  INC  (hide {APPLY}) re-pairs
//       with the parent's `(b)`  ->  ((b)+1).
//
// Fold-resistance: every macro operand reaches the callees as a FUNCTION
// ARGUMENT seeded through `ident` (an opaque pass-through the optimizer cannot
// fold to a constant at -O0), so the baseline arm keeps live runtime arithmetic
// over the cross-boundary-expanded values. The PRIMARY witness is still
// compile-time: a frozen (un-re-paired) `SQ`/`INC` leaves a bare function-like
// name with a stray `(...)` -- a syntax error -- and the program never links.
//
//   a = ident(6)            = 6
//   NAME(a)   = SQ(a)       = (a)*(a)   = 36          (object -> function-like)
//   b = ident(5)            = 5
//   APPLY(INC)(b) = INC(b)  = (b)+1     = 6           (function-like -> f-like)
//   result = 36 + 6         = 42                       -> exit 42
#define SQ(x)    ((x) * (x))
#define NAME     SQ
#define INC(x)   ((x) + 1)
#define APPLY(f) f

int ident(int x) {
    return x;
}

int main(void) {
    int a = ident(6);
    // Cross-boundary: NAME expands (object) to SQ, which re-pairs with `(a)`.
    int sq = NAME(a);

    int b = ident(5);
    // Cross-boundary: APPLY(INC) yields INC, which re-pairs with `(b)`.
    int inc = APPLY(INC)(b);

    return sq + inc;
}
