// P68 round 8 (lane `ht`): a `const` REACHED THROUGH A TYPEDEF folds in an array
// dimension exactly as its direct spelling does.
//
// `typedef const int CI;` hands `CI ROWS = 4;` a type that carries no trace of the
// const (DSS does not intern `const`), so until the typedef's own claim was applied
// `ROWS` was an ordinary MUTABLE int: the file-scope `int table[ROWS][COLS];` below
// was refused as a variably modified type at file scope, while its direct twin
// `const int ROWS = 4;` folded. ✔MEASURED 2026-09-23: clang 18.1.3 folds BOTH
// spellings in its default mode (a -Wgnu-folding-constant warning), gcc 13.3.0
// refuses both — so by the union the fold is owed, and owed to both spellings
// alike. `typedef CI CI2;` carries the claim on through a chain.
//
// The contents are argc-seeded, so the release optimizer cannot fold the sum
// away; only the dimensions are translation-time constants. Every term is
// data-model independent, so the one exit code holds on all four targets.
//
// exit = sizeof table / sizeof table[0][0] (12) + sum (66) - 36 = 42,
//        with argc == 1, so sum = 0 + 1 + ... + 11.
typedef const int CI;
typedef CI CI2;

CI ROWS = 4;
CI2 COLS = 3;

static int table[ROWS][COLS];

int main(int argc, char **argv) {
    (void)argv;
    int sum = 0;
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            table[r][c] = (r * COLS + c) * argc;
            sum += table[r][c];
        }
    }
    return (int)(sizeof table / sizeof table[0][0]) + sum - 36;
}
