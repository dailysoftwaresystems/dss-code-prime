/* A BIT-FIELD'S SIGN IS THE PAIR'S: a plain `char` field and an enumeration field.
 *
 * Whether a plain `char` is signed is the target's (C 6.2.5p15) — signed on x86_64 and on
 * Apple's arm64, unsigned on arm64 GNU/Linux — and a plain `char` BIT-FIELD has that sign.
 * An enumeration field has the sign of the integer type its enumeration is compatible
 * with (C 6.7.2.2p4): `int` under the Microsoft x64 ABI, `unsigned int` for an enumeration
 * with no negative value under SysV, AAPCS and Darwin. So the program asks each answer of
 * the pair itself — `(char)-1 < 0`, `(enum E)-1 < 0` — and checks every field against it,
 * read through a constant object (which a compiler may fold) and a volatile one (which it
 * reads at run time). Each check returns its own exit code; the program exits 42 when
 * every one holds — as it does on gcc 13, clang 18, AppleClang 21, mingw-w64 gcc 13.2 and
 * cl 19.51, and on gcc 13 for arm64 GNU/Linux.
 */

enum E { EA, EB };
enum N { NM = -1, NP = 1 };

struct S {
    char b : 3;
    signed char s : 3;
    unsigned char u : 3;
};

struct T {
    enum E e : 3;
    enum N n : 3;
};

static const struct S g = { 7, 7, 7 };
static volatile struct S v = { 7, 7, 7 };
static const struct T k = { 7, 7 };
static volatile struct T t = { 7, 7 };

/* What follows the `if` is reached only where the field is not -1: a fold of the condition
 * that disagrees with the run time's read sends the run time past the end of the function. */
static int pick(void) {
    if (g.b == -1) return 42;
    return 7;
}

static int pick_enum(void) {
    if ((int)k.e == -1) return 42;
    return 7;
}

int main(void) {
    int const charSigned = (char)-1 < 0;
    int const enumSigned = (enum E)-1 < 0;
    int const wantChar = charSigned ? -1 : 7;
    int const wantEnum = enumSigned ? -1 : 7;
    if (v.b != wantChar) return 1;                     /* the run time's read */
    if (g.b != wantChar) return 2;                     /* the constant's */
    if (v.s != -1 || g.s != -1) return 3;              /* signed char: on every pair */
    if (v.u != 7 || g.u != 7) return 4;                /* unsigned char: on every pair */
    if (pick() != (charSigned ? 42 : 7)) return 5;
    if ((int)t.e != wantEnum) return 6;
    if ((int)k.e != wantEnum) return 7;
    if ((int)t.n != -1 || (int)k.n != -1) return 8;    /* a negative enumerator: signed */
    if (pick_enum() != (enumSigned ? 42 : 7)) return 9;
    v.b = 6;                                           /* the bit pattern 110 */
    if (v.b != (charSigned ? -2 : 6)) return 10;
    if ((v.b < 0) != charSigned || (g.b < 0) != charSigned) return 11;
    if ((t.e < 0) != enumSigned || (k.e < 0) != enumSigned) return 12;
    if (v.b + 0 != v.b || sizeof(v.b + 0) != sizeof(int)) return 13;   /* promoted to int */
    return 42;
}
