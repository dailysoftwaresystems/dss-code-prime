// D-CSUBSET-GENERIC-RESULT-TYPE-DEDUCTION (C23 6.5.1.1p3): a generic selection's
// TYPE is the SELECTED association's, so `sizeof` of a `_Generic` — and the
// storage width of an `auto` bound to one — observe the WINNER, not the
// controlling expression. The controlling `long` selects the `long:` arm whose
// result is `(char)1`, so the selection types as `char` (width 1) on EVERY data
// model; the pre-fix code typed it from the controlling `long` (width 8 on LP64,
// 4 on LLP64), which would make this exit 56 / 48 instead of 42.
//
// argc-seeded so the controlling operand is a genuine runtime `long`.
// exit = arr[0] (40) + sizeof(r) (1) + (r == 1) (1) = 42 on every 64-bit target.
int main(int argc, char **argv) {
    (void)argv;
    long x = argc;                          // runtime long, value 1
    // auto bound to a _Generic: the object is the SELECTED `char` (sizeof 1),
    // not the controlling `long`.
    auto r = _Generic((x), long: (char)1, default: (double)2);
    // The array dimension folds `sizeof(_Generic(...))` in Pass 1.5 — 1 (the
    // selected char) + 39, so arr has 40 elements. The pre-fix fold used the
    // controlling long → 8 (LP64) / 4 (LLP64) + 39.
    int arr[sizeof(_Generic((x), long: (char)1, default: (int)2)) + 39];
    arr[0] = (int)(sizeof(arr) / sizeof(arr[0]));       // 40 (fixed)
    return arr[0] + (int)sizeof(r) + (r == 1 ? 1 : 0);  // 40 + 1 + 1 = 42
}
