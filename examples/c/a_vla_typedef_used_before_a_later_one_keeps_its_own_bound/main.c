/* A VARIABLY MODIFIED TYPEDEF USED BEFORE A SAME-NAMED ONE DECLARED LATER KEEPS ITS OWN BOUND (C 6.2.1p7, C 6.7.8p3;
 * P69 round 1, lane cs, fold f26c). An ordinary identifier's scope begins just after its declarator completes, so
 * `R a;` written before an inner block's own `typedef int R[m];` declares `a` with the OUTER typedef -- n ints --
 * and a `R b;` after it has the inner one's m ints. The size an object of a variably modified typedef takes is the
 * bound its typedef froze when it was reached: the outer typedef's for `a`, the inner one's for `b`. Every return
 * before 42 names the first fact that failed. */
static int sink(int const *p, int k) { int s = 0; for (int i = 0; i < k; ++i) s += p[i]; return s; }

static int sizes(int n, int m) {
    typedef int R[n];
    {
        R a;                      /* the outer R: n ints */
        typedef int R[m];
        R b;                      /* the inner R: m ints */
        if (sizeof a != (unsigned)n * sizeof(int)) return 1;
        if (sizeof b != (unsigned)m * sizeof(int)) return 2;
        for (int i = 0; i < n; ++i) a[i] = i + 1;          /* 1 + 2 + 3 = 6 */
        for (int i = 0; i < m; ++i) b[i] = 10;             /* 5 * 10 = 50 */
        if (sink(a, n) != 6) return 3;
        if (sink(b, m) != 50) return 4;
        return 42;
    }
}

int main(void) {
    volatile int n = 3, m = 5;    /* runtime bounds: no constant fold */
    return sizes(n, m);
}
