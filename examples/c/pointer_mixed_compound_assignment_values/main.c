/* The two pointer-mixed compound assignments gcc BUILDS (with its int-conversion
 * warning) and C refuses (6.5.17.3p1), computed as gcc computes them: `E1 op= E2`
 * is `E1 = E1 op E2` with the BINARY operator's own value, converted to E1's type.
 *
 *   x += p   ->  x = (long long)(p + x)   the address, STRIDE-SCALED by sizeof(int)
 *   p -= q   ->  p = (int *)(p - q)       the ELEMENT difference, not an address
 *
 * Each shape adds its share to `sum` only when the stored value is gcc's; exit 42 =
 * all five. Statement position and value position both, an array right operand, and
 * a const-qualified pointee on the right of `-=`. */
static int a[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

int main(void) {
    int sum = 0;

    long long x = 3;
    int *p = a;
    x += p;
    if (x == (long long)(a + 3)) sum += 8;

    long long y = 2;
    y += a;
    if (y == (long long)&a[2]) sum += 8;

    long long z = 1;
    long long w = (z += p);
    if (w == (long long)(a + 1) && z == w) sum += 8;

    int *pp = a + 5;
    int *qq = a + 2;
    pp -= qq;
    if ((long long)pp == 3) sum += 8;

    int *vp = a + 7;
    const int *cq = a + 1;
    long long got = (long long)(vp -= cq);
    if (got == 6 && (long long)vp == 6) sum += 10;

    return sum;
}
