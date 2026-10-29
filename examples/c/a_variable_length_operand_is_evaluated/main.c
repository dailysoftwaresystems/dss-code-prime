/* P69 L2a (lane lm): what C evaluates in a variable-length operand or a variably modified type
 * name is evaluated, once, and nothing else is.
 *
 * A sizeof whose operand has variable-length array type EVALUATES that operand (C 6.5.3.4p2):
 * an index with a side effect, a call, a pointer stepped on the way - and still answers the
 * size the operand's root froze. A cast or a compound literal whose type name is variably
 * modified evaluates that type name's size expressions (C 6.7.6.2p5; a typeof of a variably
 * modified operand, C23 6.7.2.5p4). A fixed type, a variable-length array's fixed row or
 * element, a comma that decays, a nested sizeof, alignof, and a sizeof of a pointer type are
 * NOT evaluated. Exit 42, or the number of the first check that fails. */

static int calls;
static int at(void) {
    calls++;
    return 0;
}

/* 1-6: a ROW of a variable-length array, its index with a side effect. */
static int rows(int n, int m, int o) {
    int k = 0, j = 0;
    int v[n][m];
    int w[n][m][o];
    int a[5][m];
    v[0][0] = 0;
    if (sizeof v[k++] != (unsigned)m * sizeof(int) || k != 1) return 1;
    if (sizeof v[at()] != (unsigned)m * sizeof(int) || calls != 1) return 2;
    k = 0;
    if (sizeof (v[k++]) != (unsigned)m * sizeof(int) || k != 1) return 3;
    k = 0;
    if (sizeof v[k = 1] != (unsigned)m * sizeof(int) || k != 1) return 4;
    k = 0;
    if (sizeof w[k++][j++] != (unsigned)o * sizeof(int) || k != 1 || j != 1) return 5;
    k = 0;
    if (sizeof a[k++] != (unsigned)m * sizeof(int) || k != 1) return 6;
    return 42;
}

/* 7-10: the pointee of a POINTER to a variable-length array, the pointer stepped on the way. */
static int pointers(int n, int m) {
    int k = 0;
    int v[n][m];
    int (*p)[m] = v;
    if (sizeof *(p++) != (unsigned)m * sizeof(int) || p != &v[1]) return 7;
    p = v;
    if (sizeof *p++ != (unsigned)m * sizeof(int) || p != &v[1]) return 7;
    p = v;
    if (sizeof *++p != (unsigned)m * sizeof(int) || p != &v[1]) return 8;
    p = v;
    if (sizeof p++[0] != (unsigned)m * sizeof(int) || p != &v[1]) return 9;
    p = v;
    if (sizeof p[k++] != (unsigned)m * sizeof(int) || k != 1) return 10;
    return 42;
}

/* 11-13: a typeof of a row in a type name, a sizeof in a loop, the type-name form (control). */
static int typeNames(int n, int m) {
    int k = 0;
    int v[n][m];
    unsigned t = 0;
    if (sizeof(typeof(v[k++])) != (unsigned)m * sizeof(int) || k != 1) return 11;
    k = 0;
    for (int i = 0; i < 3; i++) t += (unsigned)sizeof v[k++];
    if (k != 3 || t != 3u * (unsigned)m * sizeof(int)) return 12;
    k = 0;
    if (sizeof(int[k++ + 1]) != sizeof(int) || k != 1) return 13;
    return 42;
}

/* 14-16: a cast's and a compound literal's variably modified type name is evaluated. */
static int castsAndLiterals(int n, int m) {
    int k = 0;
    int v[n][m];
    int x[4] = {0};
    (void)(int (*)[k++ + 1])x;
    if (k != 1) return 14;
    k = 0;
    (void)(typeof(v[k++]) *)x;
    if (k != 1) return 15;
    k = 0;
    (void)(int (*)[k++ + 1]){ (void *)x };
    if (k != 1) return 16;
    return 42;
}

/* 17-26: NOT a variable-length array type - NOT evaluated; and the bare object's size. */
static int unevaluated(int n, int m) {
    int k = 0;
    int f[3][1];
    int r[n][1];
    int v[n][m];
    int u[n];
    if (sizeof f[k++] != sizeof(int) || k != 0) return 17;
    if (sizeof r[k++] != sizeof(int) || k != 0) return 18;
    if (sizeof v[k++][0] != sizeof(int) || k != 0) return 19;
    if (sizeof (k++, u) != sizeof(int *) || k != 0) return 20;
    if (sizeof(sizeof v[k++]) != sizeof(sizeof(int)) || k != 0) return 21;
    if (_Alignof(int[k++ + 1]) != _Alignof(int) || k != 0) return 22;
    if (__alignof__(v[k++]) != __alignof__(int) || k != 0) return 23;
    if (_Alignof(typeof(v[k++])) != _Alignof(int) || k != 0) return 24;
    if (sizeof(typeof(v[k++]) *) != sizeof(int *) || k != 0) return 25;
    n = 99;
    if (sizeof u != 4 * sizeof(int) || sizeof (u) != sizeof u) return 26;
    return 42;
}

int main(void) {
    int r;
    if ((r = rows(4, 3, 2)) != 42) return r;
    if ((r = pointers(4, 3)) != 42) return r;
    if ((r = typeNames(4, 3)) != 42) return r;
    if ((r = castsAndLiterals(4, 3)) != 42) return r;
    if ((r = unevaluated(4, 3)) != 42) return r;
    return 42;
}
