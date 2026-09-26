// P68 round 12 (lane `cs`) — the integer type an ENUMERATED TYPE is compatible with (C
// 6.7.2.2p4; C23 6.7.3.3p13) under the Microsoft x64 convention the pe64 formats declare
// (`enumCompatibleTypeRule: "msvc"`, MSVC's documented rule): `int` while `int` holds
// every value — a non-negative enumeration included. The choice is observable: the
// enumeration's arithmetic and bit-fields are `int`'s (signed), a redeclaration may spell
// `int`, a pointer to it converts to `int *` silently, and `_Generic` selects `int:` (C
// 6.5.1.1p3 with MSVC's own compatible type). `bad` counts the checks that fail.

enum Pos { P0, P1 = 1, P2 = 2, P3 = 3 };          // no negative value: still int
enum Neg { NM = -1, N42 = 42 };                   // a negative value: int

// Redeclarations across an enumeration and its compatible type (C 6.2.7p1, 6.7p4).
int f_pos(void);
enum Pos f_pos(void) { return P3; }
int f_neg(void);
enum Neg f_neg(void) { return N42; }
extern int v_pos;
enum Pos v_pos = P2;

struct Flags { enum Pos f : 2; };

static int check(void) {
    int bad = 0;
    enum Pos p = P1;
    enum Neg n = NM;

    bad += _Generic(p, unsigned int: 1, int: 2, default: 3) != 2;
    bad += _Generic(n, unsigned int: 1, int: 2, default: 3) != 2;
    bad += _Generic(&p, int *: 1, default: 3) != 1;

    // A pointer to the enumeration converts to `int *` with no cast.
    int *pp = &p;
    int *pn = &n;
    bad += *pp != 1 || *pn != -1;

    // The enumeration's own arithmetic is `int`'s, even with no negative value.
    bad += (p > -1) != 1;
    bad += (n > -1) != 0;

    // `e - 1` for `e == 0` is `int` arithmetic: -1, in a `long` and in a `long long`. Under
    // the `gnu` convention it is 4294967295, which this LLP64 target's 32-bit `long` wraps
    // to -1 as well: the `long long` is the check that tells the two conventions apart here.
    enum Pos e = P0;
    long d = e - 1;
    long long dd = e - 1;
    bad += d != -1L || dd != -1LL;

    // A two-bit bit-field of it is SIGNED: 3 does not fit, and reads back as -1.
    struct Flags s;
    s.f = P3;
    bad += (int)s.f != -1;

    bad += f_pos() != 3 || f_neg() != 42 || v_pos != 2;
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
