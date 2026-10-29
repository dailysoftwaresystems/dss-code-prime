/* P69 L2 (lane lm): PARENTHESISED CALL CHAINS, 30 LEVELS EACH. A `(` opens a speculation whose cast
 * reading the parser must refute; while each level retried its inner region both ways the parse grew
 * x1.62 per level (MEASURED 2026-10-20: 9245 ms at 24 levels, ~100 s INFERRED at 30). Every chain
 * asks the binder sketch at every level but the first. Exit 42, or the number of the first chain
 * that answers wrong. */
typedef int T;
typedef int (*FP)(int);
typedef int G;
static int g(int v) { return v; }
static int h(int v) { return v; }
static FP fp = h;

/* 1: a call chain inside parentheses. */
static int parenCall(void) { return (g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(g(42))))))))))))))))))))))))))))))); }
/* 2: a cast to a typedef around each call. */
static int castCall(void) { return (T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g((T)(g(42)))))))))))))))))))))))))))))))))))))))))))))))))))))))))))); }
/* 3: a function-pointer OBJECT called through parentheses. */
static int fnptrCall(void) { return (fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)((fp)(42)))))))))))))))))))))))))))))); }
/* 4: a cast to a function-pointer TYPE as the callee. */
static int fnptrTypeCall(void) { return ((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(((FP)(h))(42)))))))))))))))))))))))))))))); }
/* 5: an object that shadows a typedef, called through parentheses. */
static int shadowCall(void) { int (*G)(int) = g; return (G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)((G)(42)))))))))))))))))))))))))))))); }

int main(void) {
    if (parenCall() != 42) return 1;
    if (castCall() != 42) return 2;
    if (fnptrCall() != 42) return 3;
    if (fnptrTypeCall() != 42) return 4;
    if (shadowCall() != 42) return 5;
    return 42;
}
