// P68 round 10 (lane `cs`) — C 6.7.6.3p7: an array parameter is adjusted to a
// QUALIFIED pointer whose qualifiers are the ones written inside its outermost
// brackets, so `int p[volatile]` declares exactly `int *volatile p`. Two rules decide
// whether that type is usable, and both are checked here too: C 6.7.6.3p15 — a
// FUNCTION type takes each parameter's unqualified type, so `sum` below is an
// `int (int *)` however its parameter is spelled — and C 6.7.3p10 — a qualifier on a
// typedef'd ARRAY qualifies its ELEMENT. Only `_Generic` can see these types, so it
// is what counts; `bad` counts the checks that fail.

typedef int A[2];

static int bracket(int p[volatile]) {
    return _Generic(&p, int *volatile *: 1, int **: 2, default: 3);
}
static int bracket_const(int p[const volatile]) {
    return _Generic(&p, int *const volatile *: 1, int *volatile *: 2, int **: 3, default: 4);
}
static int bracket_static(int p[static volatile 2]) {
    return _Generic(&p, int *volatile *: 1, int **: 2, default: 3);
}
static int bracket_rows(int p[volatile][2]) {
    return _Generic(&p, int (*volatile *)[2]: 1, int (**)[2]: 2, default: 3);
}
static int bracket_vla(int n, int p[volatile n]) {
    return _Generic(&p, int *volatile *: 1, int **: 2, default: 3) + 0 * n;
}
// the pointer is volatile, not const: it can move
static int walk(int p[volatile]) {
    p++;
    return *p;
}
// a typedef'd array's head qualifier is the ELEMENT's: `p` points at `volatile int`
static int typedef_head(volatile A p) {
    return _Generic(&p, volatile int **: 1, int *volatile *: 2, int **: 3, default: 4);
}

static int sum(int p[volatile]) { return p[0] + p[1]; }
static int explicit_sum(int *volatile p) { return p[0] + p[1]; }

int main(void) {
    int a[2] = { 40, 2 };
    int rows[1][2] = { { 40, 2 } };
    int *pa = a;
    int bad = 0;

    bad += bracket(a) != 1;
    bad += bracket_const(a) != 1;
    bad += bracket_static(a) != 1;
    bad += bracket_rows(rows) != 1;
    bad += bracket_vla(2, a) != 1;
    bad += walk(a) != 2;
    bad += typedef_head(pa) != 1;
    bad += typedef_head(a) != 1;   // the array decays to `int *`, which gains the `volatile`

    // C 6.7.6.3p15: both functions ARE `int (int *)`.
    int (*fp)(int *) = sum;
    int (*gp)(int *) = explicit_sum;
    bad += _Generic(sum, int (*)(int *): 0, default: 1);
    bad += _Generic(explicit_sum, int (*)(int *): 0, default: 1);
    bad += fp(a) + gp(a) != 84;

    // C 6.7.3p10: a local of the typedef'd array type holds `volatile int`s.
    volatile A x = { 40, 2 };
    bad += _Generic(&x[0], volatile int *: 0, default: 1);
    bad += _Generic(x, volatile int *: 0, default: 1);
    bad += _Generic(&x, volatile int (*)[2]: 0, default: 1);
    bad += x[0] + x[1] != 42;

    return bad == 0 ? 42 : 1;
}
