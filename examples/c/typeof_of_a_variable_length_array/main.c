/* P69 L2 (lane lm): A `typeof` OF A VARIABLE-LENGTH ARRAY TYPE keeps its run-time
 * evaluation. The bound of `typeof(int[n + k++])` is evaluated when the declaration
 * is reached, ONCE however many names the declaration has, through nested typeof
 * layers too, and a typedef spelled with it freezes the size there (C23 6.7.2.5,
 * 6.7.8p3) — also when the typedef adds dimensions of its own, over the head or over
 * a variable-length typedef, and when the typeof names such a typedef; and a typeof
 * of a variable-length array OBJECT read whole takes the size the object froze. Each
 * check counts its own `k`. Exit 42, or the number of the first check that fails. */

/* 1: one layer — 4 elements, the bound evaluated once. */
static int one(int n) {
    int k = 0;
    typeof(int[n + k++]) a;
    return (int)(sizeof a / sizeof a[0]) * 10 + k;
}

/* 2: three nested layers. */
static int layers(int n) {
    int k = 0;
    typeof(typeof(typeof(int[n + k++]))) a;
    return (int)(sizeof a / sizeof a[0]) * 10 + k;
}

/* 3: two names in one declaration — the bound evaluated ONCE, both of its size. */
static int two(int n) {
    int k = 0;
    typeof(int[n + k++]) a, b;
    a[3] = 7;
    b[3] = 8;
    return (int)(sizeof a / sizeof a[0]) * 10 + (int)(sizeof b / sizeof b[0]) + k
           + (a[3] + b[3] == 15 ? 0 : 100);
}

/* 4: a pointer to the type: the pointee's size is the bound's. */
static int pointer(int n) {
    int k = 0;
    int arr[8] = { 0 };
    typeof(int[n + k++]) *p = (void *)arr;
    (*p)[2] = 5;
    return (int)(sizeof *p / sizeof (*p)[0]) * 10 + k + (arr[2] == 5 ? 0 : 100);
}

/* 5: the declarator's own dimension above it: `a` is m arrays of the type. */
static int own(int n, int m) {
    int k = 0;
    typeof(int[n + k++]) a[m];
    a[m - 1][n - 1] = 3;
    return (int)(sizeof a / sizeof a[0][0]) + k + (a[m - 1][n - 1] == 3 ? 0 : 100);
}

/* 6: a typedef spelled with it freezes the size at the typedef. */
static int frozen(int n) {
    int k = 0;
    typedef typeof(int[n + k++]) R;
    n = 99;
    R a;
    return (int)(sizeof a / sizeof a[0]) * 10 + k;
}

/* 7: two typedef names in one declaration: evaluated once, both of its size. */
static int twoNames(int n) {
    int k = 0;
    typedef typeof(int[n + k++]) R, S;
    R a;
    S b;
    return (int)(sizeof b / sizeof b[0]) * 10 + k + (sizeof a == sizeof b ? 0 : 100);
}

/* 8: a typedef's own dimension over the head's first name: S is m arrays of R. */
static int ownTypedef(int n, int m) {
    int k = 0;
    typedef typeof(int[n + k++]) R, S[m];
    n = 99;
    S b;
    b[m - 1][3] = 6;
    return (int)(sizeof b / sizeof(R)) * 10 + (int)(sizeof(R) / sizeof(int)) + k
           + (b[m - 1][3] == 6 ? 0 : 100);
}

/* 9: a typeof of a variable-length TYPEDEF: R's bound, frozen at R, for a typedef
 * and for objects with and without a dimension of their own. */
static int ofAlias(int n, int m) {
    int k = 0;
    typedef int R[n + k++];
    typedef typeof(R) S;
    n = 99;
    S b;
    typeof(R) a;
    typeof(R) c[m];
    return (int)(sizeof b / sizeof b[0]) * 10 + k + (sizeof a == sizeof b ? 0 : 100)
           + (sizeof c == sizeof b * (unsigned)m ? 0 : 200);
}

/* 10: a typedef's own dimension over a typeof of a variable-length typedef. */
static int ofAliasOwn(int n, int m) {
    int k = 0;
    typedef int R[n + k++];
    typedef typeof(R) S[m];
    n = 99;
    S b;
    return (int)(sizeof b / sizeof(R)) * 10 + (int)(sizeof(R) / sizeof(int)) + k;
}

/* 11: the same without typeof: a typedef's own dimension over a VLA typedef. */
static int chainOwn(int n, int m) {
    int k = 0;
    typedef int R[n + k++];
    typedef R S[m];
    n = 99;
    S b;
    b[m - 1][3] = 2;
    return (int)(sizeof b / sizeof(R)) * 10 + (int)(sizeof(R) / sizeof(int)) + k
           + (b[m - 1][3] == 2 ? 0 : 100);
}

/* 12: a typeof of a variable-length array OBJECT read whole, every declaration form:
 * each name takes v's size, frozen at v's declaration; `n = 99` changes none. */
static int objectForms(int n, int m) {
    int v[n];
    n = 99;
    typeof(v) a, b;
    typeof(v) c[m];
    typeof(v) *p = &v;
    typedef typeof(v) T;
    T d;
    typedef typeof(v) S[m];
    S e;
    if (sizeof a != 4 * sizeof(int) || sizeof b != sizeof a) return 1;
    if (sizeof c != (unsigned)m * 4 * sizeof(int)) return 2;
    if (sizeof *p != 4 * sizeof(int)) return 3;
    if (sizeof d != 4 * sizeof(int) || sizeof(T) != 4 * sizeof(int)) return 4;
    if (sizeof e != (unsigned)m * 4 * sizeof(int) || sizeof(S) != sizeof e) return 5;
    v[3] = 1;
    c[m - 1][3] = 7;
    (*p)[3] += 7;
    e[m - 1][3] = 9;
    if (c[m - 1][3] + v[3] + e[m - 1][3] != 24) return 6;
    return 42;
}

int main(void) {
    if (one(4) != 41) return 1;
    if (layers(4) != 41) return 2;
    if (two(4) != 45) return 3;
    if (pointer(4) != 41) return 4;
    if (own(4, 10) != 41) return 5;
    if (frozen(4) != 41) return 6;
    if (twoNames(4) != 41) return 7;
    if (ownTypedef(4, 3) != 35) return 8;
    if (ofAlias(4, 3) != 41) return 9;
    if (ofAliasOwn(4, 3) != 35) return 10;
    if (chainOwn(4, 3) != 35) return 11;
    if (objectForms(4, 3) != 42) return 12;
    return 42;
}
