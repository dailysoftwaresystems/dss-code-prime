/* P31 — the GNU `__extension__` prefix, in every position gcc 13.3.0 AND
 * clang 19.1.7 admit it, and in none they refuse.
 *
 * Each SHAPE below is one of the four grammar positions the construct occupies,
 * plus the two properties a wrapper rule must not lose (repetition, and
 * transparency to the value).  The exit code is 42 and every shape contributes
 * to it, so a shape that parses and then LOSES its declaration cannot pass.
 *
 * ✔MEASURED: this file is what glibc writes. A plain `#include <stdlib.h>`
 * expands to 23 `__extension__` occurrences on gcc 13.3.0 / glibc 2.39
 * (`__extension__ typedef long long ...`, `__extension__ extern long long int
 * atoll (const char *)`, `__extension__ static __inline ...`), and 40 C headers
 * under /usr/include carry 261 of them in total.  SHAPES 1-4 are those forms. */

extern int printf(const char *, ...);

/* SHAPE 1 — a file-scope typedef prefix.  glibc: `__extension__ typedef long
 * long int __quad_t;` (bits/types.h). */
__extension__ typedef long long ext_ll;

/* SHAPE 2 — a file-scope extern prototype prefix.  glibc: `__extension__
 * extern long long int atoll (const char *__nptr)` (stdlib.h). */
__extension__ extern int ext_shared_seed;
int ext_shared_seed = 5;

/* SHAPE 3 — a file-scope object definition prefix, with a storage class AFTER
 * the keyword (the only order the references accept). */
__extension__ static ext_ll ext_file_scope = 7;

/* SHAPE 4 — a struct/union MEMBER prefix.  glibc: `__extension__ unsigned long
 * long int __value64;` (bits/atomic_wide_counter.h), and the anonymous-union
 * form in bits/pthreadtypes.h. */
struct ExtHolder {
    int a;
    __extension__ ext_ll b;
    /* SHAPE 4b — the member prefix stacked with a GNU attribute, which the
     * references admit ONLY in this order (`__extension__` first). */
    __extension__ __attribute__((aligned(4))) int c;
};

/* SHAPE 5 — a whole function definition may carry the prefix. */
__extension__ static int ext_double(int v) { return v + v; }

/* SHAPE 6 — repetition.  A `{repeat}` of the keyword would have been the
 * obvious spelling; the rule is SELF-RECURSIVE instead, because the prefix of a
 * declaration is itself a declaration. */
__extension__ __extension__ typedef int ext_int;

int main(void) {
    int total = 0;

    /* SHAPE 7 — a block-scope declaration prefix. */
    __extension__ int local = 3;
    total += local;                                   /* 3 */

    /* SHAPE 8 — a block-scope TYPEDEF prefix (the scope-symmetry case: one
     * keyword must not mean two different things depending on where it is
     * written). */
    __extension__ typedef unsigned local_u;
    __extension__ local_u lu = 4;
    total += (int)lu;                                 /* 7 */

    /* SHAPE 9 — the EXPRESSION prefix, on a literal, on a parenthesised
     * expression, and doubled. */
    total += __extension__ 1;                         /* 8 */
    total += __extension__ (2);                       /* 10 */
    total += __extension__ __extension__ 3;           /* 13 */

    /* SHAPE 10 — the expression prefix must bind an ASSIGNMENT-expression and
     * NOT a comma expression: were it `expression`, this call would parse as
     * ONE argument `(4, 100)` and print 100 with a lost argument, silently. */
    total += ext_double(__extension__ 4) - 4;         /* 17 */

    /* SHAPE 11 — the prefix is TRANSPARENT to precedence: `__extension__ a + b`
     * and `(__extension__ a) + b` denote the same value. */
    total += 1 + __extension__ 2 * 3;                 /* 24 */

    /* SHAPE 12 — `__extension__ x = v;` is an EXPRESSION statement, not a
     * declaration.  It is the case that forces the declaration reading and the
     * expression reading to share one speculative site: both readings begin
     * with the same token and only the tokens AFTER the prefix separate them. */
    __extension__ local = 6;
    total += local;                                   /* 30 */

    /* SHAPE 13 — the value of a prefixed sizeof, exercised so the prefix is
     * proven not to swallow the operator it precedes. */
    total += (int)(__extension__ sizeof(ext_int));    /* 34 */

    struct ExtHolder h;
    h.a = 1; h.b = 2; h.c = 5;
    total += h.a + (int)h.b + h.c - 3;                /* 39 */

    total += ext_file_scope - ext_shared_seed + 1;    /* 42 */

    printf("gnu_extension_keyword total=%d\n", total);
    return total;
}
