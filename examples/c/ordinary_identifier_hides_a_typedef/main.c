/* P69 L2 (lane lm): WHICH NAME A PARENTHESISED IDENTIFIER IS. An ordinary identifier
 * (an object, a function, a parameter, an enumeration constant) hides a typedef of
 * its spelling from the end of its declarator (C 6.2.1p4, p7); a struct, union or
 * enum TAG never does (C 6.2.3: tags have their own name space), and neither does a
 * structure MEMBER (its structure's own name space). Each check uses its own
 * spellings. Exit 42, or the number of the first check that fails. */

/* 1: a tag, then a typedef of its spelling — the name is the typedef. */
struct A1 { int a; };
typedef int A1;

/* 2: a typedef, then a tag of its spelling. */
typedef int B1;
struct B1 { int a; };

/* 3: a function, then a tag of its spelling — `(c1)(2)` calls the function. */
static int c1(int v) { return v + 40; }
struct c1 { int a; };

/* 4: a tag defined inside the operand never makes the callee's name a type. */
static int g1(void *p) { return p ? 1 : 42; }

typedef int D1;                            /* 5 */
typedef int E1;                            /* 6 */
typedef int F1;                            /* 7 */
typedef char H1;                           /* 9 */
typedef int I1;                            /* 10 */
typedef int J1, J2[sizeof(J1) * 10 + 2];   /* 11: the next declarator sees J1 */
typedef char K1;                           /* 12 */
typedef int L1;                            /* 13 */
typedef char M1;                           /* 14 */

/* 15: a member spelled like a typedef hides nothing — `N1 x` is the typedef. */
typedef int N1;
struct N2 { int N1; N1 x; };

/* 16: likewise in a member's bound: `(P1) 42` casts through the typedef. */
typedef int P1;
struct P2 { int P1; char a[(P1) 42]; };

/* 17: inside its own body a tag answers no ordinary lookup — `(q1)` is the function,
 * so `sizeof (q1)(2)` is `sizeof(int)`. */
static int q1(int v) { return v + 40; }
struct q1 { int a[sizeof (q1)(2)]; };

/* 13: a parameter hides the typedef in the body. */
static int l1(int L1) { return (L1) - 2; }

int main(void) {
    { A1 x = 40; struct A1 s = { 2 }; if ((A1)(x) + s.a != 42) return 1; }
    { B1 x = 40; struct B1 s = { 2 }; if ((B1)(x) + s.a != 42) return 2; }
    if ((c1)(2) != 42) return 3;
    if ((g1)((struct g1 { int a; } *)0) != 42) return 4;
    /* 5: an inner tag does not shadow the typedef. */
    { struct D1 { int a; }; D1 x = 42; if (x != 42) return 5; }
    /* 6: an object hides the typedef, and a newer TAG of its spelling changes nothing. */
    { int E1 = 44; { struct E1 { int a; }; if ((E1) - 2 != 42) return 6; } }
    /* 7: an enumeration constant hides the typedef. */
    { enum { F1 = 44 }; if ((F1) - 2 != 42) return 7; }
    /* 8: an object, then an ENUM TAG of its spelling — still the object. */
    { int G1 = 44; enum G1 { GA = 2 }; if ((G1) - GA != 42) return 8; }
    /* 9: in its own initializer the object is already in scope. */
    { int H1 = sizeof(H1); if (H1 != (int)sizeof(int)) return 9; }
    /* 10: and in the next declarator of its declaration. */
    { int I1 = 44, y = (I1) - 2; if (y != 42) return 10; }
    if (sizeof(J2) / sizeof(J1) != 42) return 11;
    /* 12: an object of an enclosing block hides the typedef in an inner one. */
    { long long K1 = 0; { int r = (int)sizeof (K1); if (r != (int)sizeof(long long)) return 12; } }
    if (l1(44) != 42) return 13;
    /* 14: `_Alignof` of an object's spelling reads the object (a GNU extension). */
    { long long M1 = 0; if (_Alignof(M1) != _Alignof(long long)) return 14; }
    { struct N2 n = { 1, 41 }; if (n.N1 + n.x != 42) return 15; }
    if (sizeof(((struct P2 *)0)->a) != 42) return 16;
    if (sizeof(((struct q1 *)0)->a) != sizeof(int) * sizeof(int) || q1(2) != 42) return 17;
    return 42;
}
