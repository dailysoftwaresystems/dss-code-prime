// FC16 (D-CSUBSET-GENERIC-SELECTION): C11/C23 6.5.1.1 generic selection.
// `tc(x)` selects an integer TAG by the controlling expression's TYPE, at
// compile time. The controlling type after lvalue conversion is matched against
// each association's type-name for compatibility; exactly one must match (or the
// `default`), and the `_Generic`'s value IS the selected association's value.
//   tc(i): i is `int`    -> 1
//   tc(d): d is `double` -> 3
//   1*10 + 3 = 13.
// Data-model-independent: `int` is 4 bytes and `double` is 8 on both LP64 and
// LLP64, so the selection is identical on every target (no `long` in a
// size-sensitive position).
#define tc(x) _Generic((x), int: 1, char: 2, double: 3, default: 0)

int main(void) {
    int    i = 0;
    double d = 0;
    return tc(i) * 10 + tc(d);
}
