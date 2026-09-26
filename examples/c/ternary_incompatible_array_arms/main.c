// P68 round 9 (lane `cs`) — D-CSUBSET-INCOMPATIBLE-ELEMENT-ARRAY-TERNARY.
//
// A conditional whose two ARRAY arms have different element types — a string
// literal (`char[3]`) beside an `int[2]` — decays both arms to pointers to
// INCOMPATIBLE types, which satisfies none of C23 6.5.16p3's pairings. gcc 13.3.0,
// clang 18.1.3, mingw-w64 13.2.0 and MSVC 19.51 all build it with a warning and run
// it (✔MEASURED 2026-09-23, every program RUN). DSS used to type the conditional as
// its FIRST arm's ARRAY and lower it as a type-confused byte copy into that
// aggregate; it now takes gcc's and clang's meaning — the conditional is a `void *`
// (MSVC gives the second arm's pointer type; the meaning fork and its decision are
// recorded on the row) — and warns S_IncompatiblePointerConversion at each.
//
// exit 42 = 0 (the else arm read back through `const int *`) + 20 (the then arm read
// back through `const char *`) + 10 (`sizeof` measures a POINTER, not a `char[3]`) +
// 12 (`_Generic` selects the `void *` association).

int main(void) {
    int arr[2] = { 42, 7 };
    int c = 0;
    const int *p = c ? "ab" : arr;
    int total = p[0] - 42;
    c = 1;
    const char *s = c ? "ab" : arr;
    total += s[1] == 'b' ? 20 : 0;
    total += sizeof(c ? "ab" : arr) == sizeof(void *) ? 10 : 0;
    total += _Generic(c ? "ab" : arr, void *: 12, default: 0);
    return total;
}
