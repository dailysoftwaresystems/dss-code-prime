/* A VARIABLE-LENGTH-array typedef reached through a QUALIFIED use. C 6.7.3p10 puts the qualifier on the ELEMENT, and
   the length stays the one the typedef froze when its declaration was reached (C17 6.7.8p3): changing `n` afterwards
   moves nothing. Each helper returns 42 when its use holds C's answer. */

/* `volatile V v`: an array of `volatile int`, of V's frozen length */
static int object(int n) {
    typedef int V[n];
    int const frozen = n;
    n = 7;
    volatile V v;
    v[0] = 40;
    v[frozen - 1] = 2;
    return (int)(sizeof v / sizeof v[0]) == frozen ? v[0] + v[frozen - 1] : 1;
}

/* `volatile V *p`: the row it steps by is V's frozen size */
static int pointer(int n) {
    typedef int V[n];
    int a[3][2] = { { 0, 0 }, { 0, 0 }, { 40, 2 } };
    volatile V *p = (volatile V *)a;
    volatile V *row = &p[2];
    return (int)(sizeof *p / sizeof (*p)[0]) == n ? (*row)[0] + p[2][1] : 1;
}

/* `volatile V arr[3]`: the object's own dimension above V's */
static int arrayOf(int n) {
    typedef int V[n];
    volatile V arr[3];
    arr[2][0] = 40;
    arr[2][n - 1] = 2;
    return (int)(sizeof arr / sizeof arr[0]) == 3 && (int)(sizeof arr[0] / sizeof arr[0][0]) == n
        ? arr[2][0] + arr[2][n - 1] : 1;
}

/* a two-dimensional alias: every level of the object carries the volatile element */
static int twoDimensional(int n, int m) {
    typedef int M[n][m];
    volatile M x;
    x[n - 1][m - 1] = 40;
    x[0][0] = 2;
    return (int)(sizeof x / sizeof x[0]) == n && (int)(sizeof x[0] / sizeof x[0][0]) == m
        ? x[n - 1][m - 1] + x[0][0] : 1;
}

int main(void) {
    if (object(2) != 42) return 1;
    if (pointer(2) != 42) return 2;
    if (arrayOf(2) != 42) return 3;
    if (twoDimensional(2, 3) != 42) return 4;
    return 42;
}
