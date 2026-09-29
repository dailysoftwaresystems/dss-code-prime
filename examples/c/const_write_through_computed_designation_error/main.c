// P68 round 9 (lane `cs`) — writes the const-write check must refuse however the
// designation reaches the object, beside the same syntax over MUTABLE objects.
//
// C 6.5.16p2 / 6.5.2.4p1 / 6.5.3.1p1: the operand written must be a MODIFIABLE
// lvalue. Two families the designation walk used to stop short of:
//   (1) `__func__`'s ELEMENTS — C 6.4.2.2p1 declares `static const char __func__[]`;
//   (2) a pointer COMPUTED on the way to a const object: `p + i`, `i + p`, `p - i`,
//       `c ? p : q`, `(z, p)`, `&a[0] + i`.
// Every write in `refused` is refused by gcc 13.3.0, clang 18.1.3 and mingw-w64
// 13.2.0; MSVC 19.51 refuses family (2) (C2166) and accepts family (1) into a
// program whose write does not read back. Every write in `accepted` is accepted by
// all four. The expectation is EXACT SET EQUALITY, so a check that refused every
// write through computed syntax would fail this entry as surely as one that
// refused none.

static int refused(int c, int z) {
    char buf[4] = "abc";
    const char *p = buf;
    const char *q = buf;
    static const char a[4] = "abc";
    __func__[0] = 'x';
    __func__[0]++;
    *__func__ = 'x';
    __func__[1] += 1;
    --__func__[0];
    *(__func__ + 1) = 'x';
    *(p + 1) = 'x';
    *(1 + p) = 'x';
    *(q + 2 - 1) = 'x';
    *(a + 1) = 'x';
    (p + 1)[0] = 'x';
    (*(p + 1))++;
    *(c ? p : q) = 'x';
    *(c ? buf : p) = 'x';
    *(z, p) = 'x';
    *(&a[0] + 1) = 'x';
    return buf[0];
}

static int accepted(int c, int z) {
    char buf[4] = "abc";
    char *m = buf;
    char *n = buf;
    const char *p = buf;
    *(m + 1) = 'x';
    *(1 + m) = 'y';
    (m + 2)[0] = 'z';
    (*(m + 1))++;
    *(c ? m : n) = 'w';
    *(z, n) = 'v';
    *(&buf[0] + 3) = 0;
    p = p + 1;
    return buf[0] + *p + (__func__[0] == 'a' ? 1 : 0);
}

int main(void) {
    return refused(1, 0) + accepted(1, 0);
}
