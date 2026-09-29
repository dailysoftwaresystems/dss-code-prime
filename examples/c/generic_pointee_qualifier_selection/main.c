/* `_Generic` selects by COMPATIBLE type, and two pointer types are compatible only
 * when what they point to is identically qualified (C 6.7.6.1p2): `int *` and
 * `const int *` are two associations, `int *volatile *` is not `int **`, a function
 * taking `const char *` is not one taking `char *`. The controlling expression is
 * lvalue-converted and decayed (C23 6.5.1.1p3) — an array or a string literal is a
 * pointer there, and its own top-level qualifiers are gone, so a `const int:`
 * association never matches an `int`. Each selection below adds 5 only when it took
 * the association C names (the last adds 2); exit 42 = all nine. */
struct S { const int *p; int arr[2]; };

static int takes_const(const char *s) { return s == 0; }

int main(void) {
    const int *cp = 0;
    int *p = 0;
    int *volatile *vpp = 0;
    static const int ca[2] = { 1, 2 };
    const struct S s = { 0, { 1, 2 } };
    int sum = 0;

    sum += _Generic(cp, int *: 0, const int *: 5, default: 100);
    sum += _Generic(p, int *: 5, const int *: 0, default: 100);
    sum += _Generic(vpp, int **: 0, int *volatile *: 5, default: 100);
    sum += _Generic(ca, int *: 0, const int *: 5, default: 100);
    sum += _Generic("ab", char *: 5, const char *: 0, default: 100);
    sum += _Generic(&takes_const, int (*)(char *): 0, int (*)(const char *): 5, default: 100);
    sum += _Generic(s.arr, int *: 0, const int *: 5, default: 100);
    sum += _Generic(*&cp, int *: 0, const int *: 5, default: 100);
    sum += _Generic(sum, const int: 100, int: 2, default: 100);

    return sum + takes_const(0) - 1;
}
