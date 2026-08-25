/* P33 (D-CSUBSET-CONSTEXPR-AGGREGATE-TYPE + D-CSUBSET-CONSTEXPR-POINTER-CAST-NULL,
 * C23 6.7.1): a `constexpr` object of ARRAY / STRUCT / UNION type, and the
 * CAST-form null pointer constant as a constexpr pointer initializer.
 *
 * Every value below is a compile-time constant, so a `constexpr` object is
 * exactly the `const` object with a folded initializer -- the invariant the
 * feature rests on. The tail is ARGC-SEEDED so the release pipeline cannot fold
 * the whole program to its answer: `n` is a runtime value and the loop reads the
 * table through it.
 *
 * exit = 17 + 10 + 4 + 4 + 1 + 1 + 9 + 1 + 4 + 10 + 3 + 2 = 66  (for argc == 1)
 */

struct Point  { int x; int y; };
struct Handle { int *p; int tag; };
union  Box    { int i; };

constexpr int          TABLE[4]  = {2, 3, 5, 7};
constexpr struct Point ORIGIN    = {4, 6};
constexpr char         NAME[]    = "dss";
constexpr int          GRID[2][2] = {{1, 2}, {3, 4}};
constexpr union Box    BOX       = {9};

/* The two forms D-CSUBSET-CONSTEXPR-POINTER-CAST-NULL adds: a pointer-typed cast
 * of a literal zero, and one of a folded integer constant expression. */
constexpr int *NULLP = (int *)0;
constexpr int *NULLQ = (int *)(2 - 2);

/* A cast-form null pointer constant one brace deep -- the element walk and the
 * top-level pointer arm must agree on what a constant is. */
constexpr struct Handle HANDLE = {(int *)0, 3};

int main(int argc, char **argv) {
    (void)argv;
    int total = 0;

    total += TABLE[0] + TABLE[1] + TABLE[2] + TABLE[3];      /* 17 */
    total += ORIGIN.x + ORIGIN.y;                            /* 10 */
    total += (int)sizeof(NAME);                              /*  4 */
    total += GRID[1][1];                                     /*  4 */
    total += (NULLP == 0) ? 1 : 0;                           /*  1 */
    total += (NULLQ == 0) ? 1 : 0;                           /*  1 */
    total += BOX.i;                                          /*  9 */
    total += (NAME[0] == 'd' && NAME[3] == 0) ? 1 : 0;       /*  1 */
    total += (int)sizeof(TABLE) / (int)sizeof(TABLE[0]);     /*  4 */

    /* Block scope carries the same rule. */
    constexpr int LOCAL[2] = {5, 5};
    total += LOCAL[0] + LOCAL[1];                            /* 10 */

    total += (HANDLE.p == 0) ? HANDLE.tag : 0;               /*  3 */

    int n = argc;
    if (n > 4) n = 4;
    for (int i = 0; i < n; ++i) total += TABLE[i];           /*  2 for argc == 1 */

    return total;
}
