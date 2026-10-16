/* P69 (lane `cs`) — D-C-SIZEOF-OF-A-COMPOUND-LITERAL-IS-A-PARSE-ERROR: `sizeof (int){ 7 }` is
 * `sizeof` applied to the unary-expression `(int){ 7 }` — a compound literal, a
 * postfix-expression (C 6.5.2.5) — not the `sizeof ( type-name )` form, which closes at the
 * `)` and leaves the brace list over. Each operand below is typed as the references type
 * it; exit 42 when every one measures right, otherwise a bitmask:
 *   1 a scalar literal, 2 a structure literal, 4 an unsized array literal (its OWN length),
 *   8 a postfix `[0]` and `.d` of one, 16 in an array bound and a static initializer,
 *   32 UNEVALUATED (its initializer's side effects never run), 64 a pointer literal and an
 *   arithmetic operand after it, 128 `__alignof__` of one (the GNU spelling, so only where
 *   `__GNUC__` is defined — DSS defines it), 256 the type form unchanged.
 * gcc 13.3.0, clang 18.1.3, mingw-w64 13.2.0 and MSVC 19.51 build every one. */
struct S { int a; char b[12]; };
struct D { int a; double d; };

static int bounded[sizeof (int[]){ 1, 2, 3 } / sizeof(int)];
static unsigned long chars = sizeof (char[]){ "abc" };

int main(int argc, char **argv) {
    (void)argv;
    int bad = 0;
    int n = argc - argc;
    if (sizeof (int){ 7 } != sizeof(int)) bad |= 1;
    if (sizeof (struct S){ 1 } != sizeof(struct S)) bad |= 2;
    if (sizeof (int[]){ 1, 2, 3 } != 3 * sizeof(int)
        || sizeof (char[]){ "abcde" } != 6) bad |= 4;
    if (sizeof (int[]){ 1, 2, 3 }[0] != sizeof(int)
        || sizeof (struct D){ 0 }.d != sizeof(double)) bad |= 8;
    if (sizeof bounded != 3 * sizeof(int) || chars != 4) bad |= 16;
    unsigned long s = sizeof (int[]){ n++, n++ };
    if (n != 0 || s != 2 * sizeof(int)) bad |= 32;
    if (sizeof (int *){ 0 } != sizeof(int *)
        || sizeof (int){ 7 } + 1 != sizeof(int) + 1
        || sizeof (int){ 7 } * 2 != 2 * sizeof(int)) bad |= 64;
#ifdef __GNUC__   /* `__alignof__` is the GNU spelling; MSVC has none */
    if (__alignof__ (double){ 7 } != __alignof__(double)) bad |= 128;
#endif
    if (sizeof (int) + 1 != sizeof(int) + 1) bad |= 256;
    return bad == 0 ? 42 : bad;
}
