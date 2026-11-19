/* A BIT-FIELD'S VALUE IS PROMOTED BY ITS WIDTH (C 6.3.1.1p2).
 *
 * "If an int can represent all values of the original type (as restricted by the width,
 * for a bit-field), the value is converted to an int; otherwise, it is converted to an
 * unsigned int." An `unsigned b : 3` holding 5 is the `int` 5 in an expression, so
 * `v.b - 6` is -1; an `unsigned x : 32` is an `unsigned int`, so `v.x - 6` is 4294967295.
 * Every cell below is one the four GNU-dialect references (gcc 13, clang 18, AppleClang
 * 21, mingw-w64 gcc 13.2) answer alike; each check returns its own exit code, and the
 * program exits 42 when every one holds. The objects are `volatile` (no fold: the run
 * time computes) or `const` (the static initializers and the `if` conditions fold: the
 * compiler computes) — the two must agree.
 */

struct S {
    int pad;
    unsigned b : 3;
    unsigned w : 31;
    unsigned x : 32;
    unsigned long long q : 3;
    long long n : 3;
    unsigned char c : 3;
};
struct N { int k; struct S s; };

static volatile struct S v = { 0, 5, 5, 5, 5, 3, 5 };
static volatile struct S arr[1] = { { 0, 5, 5, 5, 5, 3, 5 } };
static volatile struct N nest = { 0, { 0, 5, 5, 5, 5, 3, 5 } };
static volatile struct S *volatile p = &v;
static const struct S g = { 0, 5, 5, 5, 5, 3, 5 };
static volatile int minus6 = -6, minus1 = -1;

/* The constant tier: static initializers, folded by the compiler. */
static int s_lt = g.b - 6 < 0;
static long long s_wide = (long long)(g.b - 6);
static long long s_x = (long long)(g.x - 6);
static long long s_q = (long long)(g.q - 6);
static int s_neg = -g.b < 0;

#define IS_INT(e) _Generic((e), int: 1, default: 0)
#define IS_UINT(e) _Generic((e), unsigned int: 1, default: 0)

/* A condition the compiler folds: what follows it must be reached when C says so. */
static int folded_true(void) { if (g.b - 6 < 0) return 1; return 0; }
static int folded_false(void) { if (g.x - 6 < 0) return 1; return 0; }

int main(void) {
    /* the TYPE, by the width */
    if (!IS_INT(v.b + 0)) return 1;
    if (!IS_INT(v.w + 0)) return 2;
    if (!IS_UINT(v.x + 0)) return 3;
    if (!IS_INT(v.q + 0) || sizeof(v.q + 0) != sizeof(int)) return 4;
    if (!IS_INT(v.n + 0)) return 5;
    if (!IS_INT(v.c + 0)) return 6;
    /* the VALUE at run time, every operator */
    if (!(v.b - 6 < 0)) return 7;
    if ((long long)(v.b - 6) != -1) return 8;
    if ((long long)(v.x - 6) != 4294967295ll) return 9;
    if ((long long)(v.q - 6) != -1) return 10;
    if (!(-v.b < 0) || !(~v.b < 0)) return 11;
    if (!(v.b > -1)) return 12;
    if (!((v.b << 1) - 11 < 0)) return 13;
    if (!((minus1 ? v.b : v.b) - 6 < 0)) return 14;
    if (!((v.b - 7) / 2 < 0)) return 15;          /* -1, where unsigned would be 2147483647 */
    /* the forms that are the field's value */
    if (!(p->b - 6 < 0) || !(arr[0].b - 6 < 0) || !(nest.s.b - 6 < 0)) return 16;
    if (!((v.b) - 6 < 0) || !((0, v.b) - 6 < 0)) return 17;
    if (!(_Generic(0, int: v.b) - 6 < 0)) return 18;
    /* compound assignment, both ways: the operator computes at the promoted operands */
    {
        int x = minus6;
        v.b = 3;
        x /= v.b;
        if (x != -2) return 19;
        x = minus6;
        x %= v.b;
        if (x != 0) return 20;
        v.b = 3;
        v.b /= minus1;                 /* -3, stored into 3 bits: 5 */
        if (v.b != 5) return 21;
        v.b = 3;
        if ((int)(v.b /= minus1) != 5) return 22;
        v.b = 5;
    }
    /* the constant tier agrees with the run time */
    if (s_lt != 1 || s_wide != -1 || s_x != 4294967295ll || s_q != -1 || s_neg != 1) return 23;
    if (folded_true() != 1 || folded_false() != 0) return 24;
    if (!(g.b - 6 < 0) || (long long)(g.b - 6) != -1) return 25;
    return 42;
}
