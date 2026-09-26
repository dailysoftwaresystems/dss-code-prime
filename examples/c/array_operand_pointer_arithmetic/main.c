// P68 round 9 (lane `cs`): an ARRAY operand of `+` / `-` decays to a pointer to its
// first element (C 6.3.2.1p3), on EITHER side of `+` — so `a + 1`, `1 + a` and `a - 0`
// are `int *`, and `1 + "never"` is `char *`. `bad` counts the checks that fail.
static int second(int const *p) { return p[0]; }

int main(void) {
    int a[10] = { 5, 7, 9 };
    int bad = 0;

    bad += sizeof(a + 1) != sizeof(int *);           // a pointer, not the array
    bad += sizeof(1 + a) != sizeof(int *);           // a pointer, not the integer
    bad += sizeof(a - 0) != sizeof(int *);
    bad += _Generic(1 + a, int *: 0, default: 1);    // its TYPE is the element pointer
    bad += _Generic(a + 1, int *: 0, default: 1);
    bad += _Generic(1 + "never", char *: 0, default: 1);

    char const *s = 1 + "never";                     // both operand orders, as pointers
    char const *t = "never" + 1;
    bad += s[0] != 'e' || s[3] != 'r' || t[0] != 'e' || t[1] != 'v';
    bad += second(1 + a) != 7 || second(a + 2) != 9 || *(1 + a) != 7;

    int x = 1 + "never";                             // a POINTER into an int: diagnosed
    bad += x == 0;

    return bad == 0 ? 42 : bad;
}
