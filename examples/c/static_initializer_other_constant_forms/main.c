// P68 round 13 (lane `cs`, the static-initializer item) — the OTHER constant forms C 6.6p10
// lets an implementation accept in a static initializer that gcc, clang and mingw-w64 build,
// each named in c.lang.json's `semantics.staticInitializers.otherConstantForms`: a const
// object's value, its element or its member (`constObjectRead`); an address's truth value
// (`addressTruthValue`); comparisons of addresses (`addressComparison`); the difference of two
// addresses into one object (`addressDifference`). Each is folded into static data, never
// computed at load time. `bad` counts the checks that fail.

struct S { int k; int m; };

int a[4] = { 1, 42, 3, 4 };
int b[2];

static const int base = 40;
static int sum = base + 2;                          // a const object's value
static const int ca[2] = { 1, 42 };
static int elem = ca[1];                            // ... an element of it
static const struct S cs = { 1, 42 };
static int memb = cs.m;                             // ... a member of it
int *const cp = &a[1];
int *fromConst = cp;                                // ... a const pointer's value
static int ch = "*abc"[0];                          // ... an element of a string literal

_Bool truth = &a;                                   // an address is true
int notNull = !&a[0];                               // 0
int cond = &a[0] ? 42 : 1;                          // selects on it
int andAlso = &a[0] && 1;                           // 1
int less = &a[1] < &a[2];                           // 1: within one object
int same = &a[1] == &a[1];                          // 1
int apart = &a[0] == &b[0];                         // 0: two objects are unequal
long diff = &a[3] - &a[1];                          // 2 elements
long bytes = (char *)&a[1] - (char *)&a[0];         // sizeof(int) bytes

static int check(void) {
    int bad = 0;
    bad += sum != 42 || elem != 42 || memb != 42 || *fromConst != 42 || ch != '*';
    bad += truth != 1 || notNull != 0 || cond != 42 || andAlso != 1;
    bad += less != 1 || same != 1 || apart != 0;
    bad += diff != 2 || bytes != (long)sizeof(int);
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
