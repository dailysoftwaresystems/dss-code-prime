/* [[D-FFI-OFFSETOF-MACRO]] — GNU `__builtin_offsetof(type-name, member-designator)`.
 *
 * ★★★ WHY THIS IS THE HIGHEST-IMPACT ROW IN THE DIRECTION-A CENSUS, AND IT IS
 * NOT BECAUSE PROGRAMS WRITE THIS TOKEN. ✔MEASURED by reading `-E` output on
 * this host, gcc 13.3.0 AND clang 19.1.1: `<stddef.h>`'s `offsetof(struct S, b)`
 * expands LITERALLY to `__builtin_offsetof(struct S, b)`. The classic
 * `((size_t)&((T*)0)->m)` text this feature was once sized around is not what a
 * real SDK header hands a compiler any more — so a translation unit that reaches
 * ANY real header trips this token without the program ever writing it.
 * ✔MEASURED through the shipped CLI at HEAD `60198126`, immediately before this
 * cycle: `error[P0002] expected 'ParenClose' — got 'struct'`. DSS was reading it
 * as an ordinary CALL, which it cannot be: `struct S` is not an expression, and
 * `b` names a MEMBER — a name that exists only inside the composite's own member
 * scope and resolves to nothing through the identifier path.
 *
 * ★★ SHAPE 6 IS THE ONE THAT MAKES A WRONG ANSWER VISIBLE AS A WRONG PROGRAM.
 * Every other shape compares the offset to a NUMBER, so a wrong offset shows up
 * as a failed comparison. Shape 6 USES the offset the way real code does — it
 * walks `(char *)&obj + offset` and reads the field back — so a wrong offset
 * produces a wrong VALUE from a program that built cleanly and ran to
 * completion. That is the failure class this construct actually threatens, and a
 * corpus that only compared integers would not have contained it.
 *
 * ★★ SHAPE 7 IS THE PROOF THAT THE ANSWER COMES FROM THE REAL LAYOUT ENGINE AND
 * NOT FROM A RE-DERIVATION. `struct Packed` and `struct Plain` have IDENTICAL
 * member lists and DIFFERENT offsets (1 vs 4, 5 vs 8), and the only thing that
 * separates them is `__attribute__((packed))`. An implementation that summed
 * member sizes itself — the obvious wrong way to write this — answers the plain
 * layout for both and passes every other shape in this file.
 *
 * ★ SHAPE 8 IS THE CONST-EXPR HALF, AND IT IS THE CANONICAL USE: a real header
 * writes `_Static_assert(offsetof(...) == N, ...)` far more often than it writes
 * a runtime offset. It routes through a different consumer (the CST const-eval
 * engine at Pass 1.5) than the runtime shapes do (Pass 2 + the HIR lowering), so
 * one working does not imply the other.
 *
 * ⚠ THE NEGATIVE HALF IS NOT IN THIS FILE AND THAT IS DELIBERATE — a corpus
 * example must BUILD. The refusals (`nosuch` is not a member; a BIT-FIELD has no
 * byte offset; a non-constant `[index]`) are pinned by the unit suite
 * `analysis/semantic/test_builtin_compile_time_operators`, which can assert on a
 * diagnostic CODE. ✔MEASURED that gcc 13.3.0, clang 18.1.3 and clang 19.1.1
 * reject the same two shapes.
 */

struct Inner { int x; int y; };
struct Outer { int p; struct Inner q; int r[4]; };
union  Both  { int i; double d; char c[16]; };
typedef struct { int h; union Both u; } Wrapped;

struct __attribute__((packed)) Packed { char c; int i; double d; };
struct Plain                          { char c; int i; double d; };

struct Basic { int a; int b; double c; };

/* SHAPE 8 — the const-expr half: a `_Static_assert` and an array dimension, the
 * two positions a header actually uses. `dss_dim` is `int[4]`. */
_Static_assert(__builtin_offsetof(struct Basic, c) == 8,
               "offsetof must fold in a constant expression");
int dss_dim[__builtin_offsetof(struct Basic, b)];

/* SHAPE 6's subject. Not `const`, and its `b` is not 0, so reading the wrong
 * field yields a DIFFERENT number rather than the same zero. */
static struct Basic dss_obj = { 1, 7, 3.0 };

volatile int dss_seed = 3;

int main(void) {
    char *base;
    int   got;
    int   seed;

    seed = dss_seed;

    /* SHAPE 1 — the flat cases, including the one where PADDING is the answer:
     * `c` is at 8 and not at 8-minus-something, because `int a; int b; double c;`
     * pads to the double's alignment. */
    if (__builtin_offsetof(struct Basic, a) != 0u) return 1;
    if (__builtin_offsetof(struct Basic, b) != 4u) return 2;
    if (__builtin_offsetof(struct Basic, c) != 8u) return 3;

    /* SHAPE 2 — a NESTED `.field` designator. The offsets compose: `q` is at 4,
     * `y` is 4 into `q`. */
    if (__builtin_offsetof(struct Outer, q.y) != 8u) return 4;

    /* SHAPE 3 — an `[index]` designator step. `r` is at 12, and index 2 of an
     * `int[4]` adds 8. A step that ignored the element STRIDE would answer 12
     * or 14, both of which this line rejects. */
    if (__builtin_offsetof(struct Outer, r[2]) != 20u) return 5;

    /* SHAPE 4 — a UNION member (always 0) and a TYPEDEF'd container, proving the
     * type position really is the shared `castTypeRef` chokepoint rather than a
     * struct-only special case. */
    if (__builtin_offsetof(union Both, d) != 0u) return 6;
    if (__builtin_offsetof(Wrapped, u) != 8u) return 7;

    /* SHAPE 5 — a MIXED designator chain: a member, then a field, then an index. */
    if (__builtin_offsetof(Wrapped, u.c[3]) != 11u) return 8;

    /* SHAPE 6 — THE OFFSET USED AS AN OFFSET. See the header note. */
    base = (char *)&dss_obj;
    got  = *(int *)(base + __builtin_offsetof(struct Basic, b));
    if (got != 7) return 9;

    /* SHAPE 7 — packed vs plain: identical members, different layouts. */
    if (__builtin_offsetof(struct Packed, i) != 1u) return 10;
    if (__builtin_offsetof(struct Packed, d) != 5u) return 11;
    if (__builtin_offsetof(struct Plain,  i) != 4u) return 12;
    if (__builtin_offsetof(struct Plain,  d) != 8u) return 13;

    /* SHAPE 8 — the array dimension really is 4 elements. */
    if ((int)(sizeof dss_dim / sizeof dss_dim[0]) != 4) return 14;

    /* Every mismatch above already returned its own code; this arithmetic is a
     * second, weaker check kept so the exit code is a FUNCTION of the measured
     * offsets rather than a constant a miscompile could still produce.
     * 4 + 8 + 20 + 8 + 1 == 41, plus `got` (7) minus the seed's 3 twice == 42. */
    return (int)__builtin_offsetof(struct Basic, b)
         + (int)__builtin_offsetof(struct Outer, q.y)
         + (int)__builtin_offsetof(struct Outer, r[2])
         + (int)__builtin_offsetof(Wrapped, u)
         + (int)__builtin_offsetof(struct Packed, i)
         + got
         - seed
         - dss_seed;
}
