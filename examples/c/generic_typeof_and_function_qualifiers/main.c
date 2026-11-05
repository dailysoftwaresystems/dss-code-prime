/* P69 (lane `cs`) — D-C-GENERIC-MATCHES-FUNCTION-TYPES-DIFFERING-IN-A-POINTEE-CONST and its
 * riders D-C-TYPEOF-DROPS-ITS-OPERAND-QUALIFIERS and
 * D-C-A-FUNCTION-TYPEDEFS-PARAMETER-QUALIFIERS-ARE-NOT-CLAIMED.
 *
 * `const` is not a TypeId bit in DSS, so the qualifier lives on a declaration's spine —
 * and three places lost it: a `typeof` head claimed none, a function typedef claimed no
 * parameter, and a conditional over two function pointers apart only in a parameter's
 * pointee kept the THEN arm's type. Each family below selects C's association; exit 42
 * when all of them do, otherwise a bitmask of the families that broke:
 *   1 typeof, 2 a function typedef, 4 typeof_unqual, 8 the conditional's type,
 *   16 the conditional's value.
 * gcc 13.3.0 and clang 18.1.3 (-std=c2x) exit 42; MSVC 19.51 (/std:clatest) exits 8 —
 * it gives the conditional an arm's type, the fork P68 round 9 decided for `void *`. */
static int f_plain(char *s) { return s[0] == 'x' ? 1 : 0; }
static int f_const(const char *s) { return s[0] == 'x' ? 2 : 0; }
static int f_vol(volatile char *s) { return s[0] == 'x' ? 8 : 0; }
static int f_top(char *const s) { return s[0] == 'x' ? 16 : 0; }
typedef int FC(const char *);
FC f_through;
int f_through(const char *s) { return s[0] == 'x' ? 4 : 0; }
const char *g_t;
const int g_cx = 3;
char *const g_pc = 0;

int main(int argc, char **argv) {
    (void)argv;
    int bad = 0;
    const char *t = "x";
    char *s = 0;
    const int cx = 1;
    /* 1: typeof names its operand's QUALIFIED type. */
    if (_Generic(&f_plain, __typeof__(&f_const): 1, default: 0)) bad |= 1;
    if (_Generic(&f_const, __typeof__(&f_plain): 1, default: 0)) bad |= 1;
    if (_Generic(0, __typeof__(cx): 1, default: 0)) bad |= 1;
    if (_Generic(s, __typeof__(t): 1, default: 0)) bad |= 1;
    if (!_Generic(&f_const, __typeof__(&f_const): 1, default: 0)) bad |= 1;
    /* … and its controls: a `volatile` pointee is another type too, a parameter's OWN
       top-level `const` is not part of the function type at all. */
    if (_Generic(&f_plain, __typeof__(&f_vol): 1, default: 0)) bad |= 1;
    if (!_Generic(&f_plain, __typeof__(&f_top): 1, default: 0)) bad |= 1;
    /* 2: a function typedef carries its parameters — and so does a function declared
       through it. */
    if (_Generic(&f_plain, FC *: 1, default: 0)) bad |= 2;
    if (!_Generic(&f_const, FC *: 1, default: 0)) bad |= 2;
    if (!_Generic(&f_through, int (*)(const char *): 1, int (*)(char *): 0, default: 0)) bad |= 2;
    /* 4: typeof_unqual drops the operand's OWN qualifiers and nothing else. */
    if (!_Generic((typeof_unqual(g_cx) *)0, int *: 1, default: 0)) bad |= 4;
    if (!_Generic((typeof_unqual(g_t) *)0, const char **: 1, char **: 0, default: 0)) bad |= 4;
    if (!_Generic((typeof_unqual(g_pc) *)0, char **: 1, default: 0)) bad |= 4;
    if (!_Generic((typeof(g_t) *)0, const char **: 1, char **: 0, default: 0)) bad |= 4;
    /* 8: a conditional over two function pointers whose parameters differ in a pointee
       `const` has type `void *` (C 6.5.16p3 admits no such pairing). */
    if (!_Generic(argc > 0 ? &f_plain : &f_const, void *: 1, default: 0)) bad |= 8;
    if (!_Generic(argc > 0 ? f_plain : f_const, void *: 1, default: 0)) bad |= 8;
    /* 16: … and its value is still the selected function's address. */
    void *v = argc > 0 ? &f_plain : &f_const;
    int (*back)(char *) = (int (*)(char *))v;
    char b[] = "x";
    if (back(b) + f_const(t) + f_through(t) + f_vol(b) + f_top(b) != 31) bad |= 16;
    return bad == 0 ? 42 : bad;
}
