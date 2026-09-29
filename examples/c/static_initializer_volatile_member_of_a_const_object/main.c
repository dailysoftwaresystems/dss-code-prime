// P68 round 13 (fold F7) — a VOLATILE MEMBER of a const, non-volatile object does not make the
// object volatile: gcc and mingw-w64 fold a static initializer's read of it, so the union of
// the references builds this file (clang and MSVC refuse the volatile reads). An object that IS
// volatile — its type, looked through its array spine, volatile-qualified — is another matter:
// every reference refuses `cva[1]` of `static const volatile int cva[2]`, and so does DSS (the
// diagnostics corpus holds that refusal). `bad` counts the checks that fail.

struct S { volatile int v; };
struct P { int a; volatile int b; };

static const struct { volatile int v; } cs = { 42 };
int m1 = cs.v;                                          // a volatile member
static const struct { volatile int v[2]; } cv = { { 1, 42 } };
int m2 = cv.v[1];                                       // ... an element of one
static const struct S sa[2] = { { 1 }, { 42 } };
int m3 = sa[1].v;                                       // ... of an array's element
static const struct { struct { volatile int v; } in; } cn = { { 42 } };
int m4 = cn.in.v;                                       // ... two levels down
static const struct { volatile int v; int w; } cw = { 1, 42 };
int m5 = cw.w;                                          // a non-volatile sibling
static const struct P cp = { 1, 42 };
struct P whole = cp;                                    // the whole object, copied

static int check(void) {
    int bad = 0;
    bad += m1 != 42 || m2 != 42 || m3 != 42 || m4 != 42 || m5 != 42;
    bad += whole.a != 1 || whole.b != 42;
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
