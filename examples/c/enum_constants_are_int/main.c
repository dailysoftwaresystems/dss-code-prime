// P68 round 12 (lane `cs`) — the type of an ENUMERATION CONSTANT (C17 6.4.4.3p2; C23
// 6.4.4.3p2 and 6.7.3.3p12-p15). A constant of an enumeration without a fixed underlying
// type is `int` when every value of its enumeration fits `int`; while its list is being
// processed it is `int` when its own value fits, else its constant expression's type, and
// an implicit `previous + 1` that leaves the previous constant's type takes a wider type
// of the previous constant's signedness. When some value does not fit `int`, the
// constants have the ENUMERATED type once the list is complete, and that type holds every
// value (no wrap to 32 bits). A constant of an enumeration WITH a fixed underlying type
// has the enumerated type, inside the list as well. `bad` counts the checks that fail;
// `llp64` selects the type names whose width differs between LP64 and LLP64.

#define KIND(X) _Generic((X), int: 1, unsigned int: 2, long: 3, unsigned long: 4, \
                              long long: 5, unsigned long long: 6, default: 9)

enum Plain { P0, P1 = 7 };                                   // every value fits int
enum Tagless { T0 = 42 };
enum During { D0 = 0x100000000, D0K = KIND(D0), D1 = 1L, D1K = KIND(D1) };
enum Implicit { I0 = 0x7fffffff, I1, I1K = KIND(I1) };       // I1 leaves int
enum ImplicitU { U0 = 0xFFFFFFFF, U1, U1K = KIND(U1) };     // U1 leaves unsigned int
enum Wide { W0 = 1, WBIG = 0x100000000, WS = sizeof(WBIG) }; // sizeof inside the list
enum Mixed { MNEG = -1, MBIG = 0xFFFFFFFF };
enum AllOnes { ALL = 0xFFFFFFFF };
enum Top { TOP = 0xFFFFFFFFFFFFFFFF };
enum Fixed : long { F0 = 1, F0K = _Generic(F0, enum Fixed: 7, default: 9) };
enum Arith { AR0 = 0x100000000, AR1 = AR0 + 1, AR2 = AR0 > 0 }; // arithmetic on a wider constant

static int check(void) {
    int const llp64 = sizeof(long) == sizeof(int);
    int bad = 0;

    // Every value fits int: the constants are `int` (all four references agree).
    bad += _Generic(P1, int: 1, default: 0) != 1;
    bad += _Generic(T0, int: 1, default: 0) != 1;
    __typeof__(P0) v = P1;
    bad += _Generic(v, int: 1, default: 0) != 1;

    // DURING the list (C23 6.7.3.3p12).
    bad += D0K != (llp64 ? 5 : 3);   // the constant expression's type: long / long long
    bad += D1K != 1;                 // a `long` expression whose value fits int is `int`
    bad += I1K != (llp64 ? 5 : 3);   // int + 1 overflows: the next SIGNED type holding it
    bad += U1K != (llp64 ? 6 : 4);   // unsigned int + 1: the next UNSIGNED type holding it
    bad += WS != 8;

    // AFTER the list: a value int cannot hold makes every constant the enumerated type,
    // which holds every value — the small `W0` too (C23 6.7.3.3p15: the member type of
    // such an enumeration IS the enumerated type; gcc 13.3.0 and mingw-w64 13.2.0 agree,
    // while clang 18.1.3 still types a fitting constant `int` there — decided by the
    // standard's text).
    bad += KIND(WBIG) == 1;
    bad += KIND(W0) == 1;
    bad += WBIG != 0x100000000;
    bad += sizeof(WBIG) != 8;
    bad += !(MBIG > 0 && MNEG < 0);
    bad += !(ALL > 0 && ALL == 0xFFFFFFFFu);
    bad += !(TOP > 0);

    // A constant wider than `int` folds at ITS OWN type inside the list (never 32 bits).
    bad += AR1 != 0x100000001;
    bad += AR2 != 1;

    // A FIXED underlying type: the enumerated type, inside the list too.
    bad += F0K != 7;
    bad += _Generic(F0, enum Fixed: 1, default: 0) != 1;
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
