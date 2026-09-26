// P68 round 12 (lane `cs`) — the integer type an ENUMERATED TYPE is compatible with (C
// 6.7.2.2p4; C23 6.7.3.3p13, p16) under the SysV / AAPCS / Darwin convention every ELF
// and Mach-O format declares (`enumCompatibleTypeRule: "gnu"`, gcc's and clang's
// documented rule): `unsigned int` when no value is negative, `int` otherwise; with a
// fixed underlying type, that type. The choice is observable: `_Generic` selects the
// compatible type's association, a pointer to one converts to a pointer to the other
// silently, a redeclaration may spell either, and the enumeration's own arithmetic and
// bit-fields take the compatible type's signedness. `bad` counts the checks that fail.

enum Pos { P0, P1 = 1, P2 = 2, P3 = 3 };          // no negative value: unsigned int
enum Neg { NM = -1, N42 = 42 };                   // a negative value: int
enum Fix : long { F42 = 42 };                     // fixed: long

// Redeclarations across an enumeration and its compatible type (C 6.2.7p1, 6.7p4).
int f_neg(void);
enum Neg f_neg(void) { return N42; }
unsigned f_pos(void);
enum Pos f_pos(void) { return P3; }
int g_fix(long x);
int g_fix(enum Fix x) { return (int)x; }
extern int v_neg;
enum Neg v_neg = N42;

struct Flags { enum Pos f : 2; };

static int check(void) {
    int bad = 0;
    enum Pos p = P1;
    enum Neg n = NM;
    enum Fix x = F42;

    bad += _Generic(p, unsigned int: 1, int: 2, default: 3) != 1;
    bad += _Generic(n, unsigned int: 1, int: 2, default: 3) != 2;
    bad += _Generic(x, long: 1, default: 3) != 1;
    bad += _Generic(&p, unsigned int *: 1, default: 3) != 1;

    // Pointers to compatible types convert with no cast.
    unsigned *pu = &p;
    int *pn = &n;
    long *px = &x;
    bad += *pu != 1u || *pn != -1 || *px != 42;

    // The enumeration's own arithmetic has its compatible type's signedness.
    bad += (p > -1) != 0;          // unsigned int: -1 converts to UINT_MAX
    bad += (n > -1) != 0;          // int: -1 > -1 is false
    enum Neg n42 = N42;
    bad += (n42 > -1) != 1;

    // `e - 1` for `e == 0` is `unsigned int` arithmetic: it wraps to 4294967295, which a
    // `long` holds on these LP64 targets. Under the `msvc` convention it is -1.
    enum Pos e = P0;
    long d = e - 1;
    bad += d != 4294967295L;

    // A two-bit bit-field of a non-negative enumeration is unsigned: it holds 3.
    struct Flags s;
    s.f = P3;
    bad += s.f != P3;

    bad += f_neg() != 42 || f_pos() != 3u || g_fix(F42) != 42 || v_neg != 42;
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
