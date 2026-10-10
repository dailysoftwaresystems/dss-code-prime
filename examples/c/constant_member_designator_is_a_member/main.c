/* THE MEMBER NAME OF A CONSTANT IS RESOLVED BY THE MEMBER RULE
 * (D-C-A-CONSTANTS-MEMBER-NAME-IS-RESOLVED-BY-A-SCOPE-WALK).
 *
 * `offsetof(T, m)`, its older spelling `&((T *)0)->m`, and the member operand of
 * the object-size builtin each ask "where in T does the member called m live?".
 * The answer is the one every member access gives: a member of T itself, or a
 * member of one of T's ANONYMOUS struct / union members — "the members of an
 * anonymous structure or union are members of the containing structure or union"
 * (C23 6.7.3.2p15), however deep — with T's qualifiers set aside.
 *
 * DSS used to answer these three with a lookup of their own, which walked the
 * scope chain outwards from T's member scope. It never looked INTO an anonymous
 * member, so every constant below that names `c` or `f` was refused ("'c' is not a
 * member of that struct/union"); and it could not find a QUALIFIED T's members at
 * all ("its containing struct/union is INCOMPLETE here"). The same walk also
 * ACCEPTED a name that is no member but is declared outside the struct — that half
 * cannot be shown by a program that builds; it is pinned as four refusals in
 * tests/analysis/semantic/test_builtin_compile_time_operators.cpp.
 *
 * Every constant here is checked against the address arithmetic on a real object,
 * so a wrong offset is a wrong exit code, on every target.
 */
#include <stddef.h>

struct A {
    char a;
    struct { int b; int c; };                              /* one anonymous level   */
    union { long long d; struct { char e; short f; }; };   /* two: struct in union  */
};
struct B { struct A in; char tail; };
struct T { char lead; struct A wide[2]; };
struct S { int a; int b; };
struct O { char lead; volatile struct S in; };

/* ── fixed while the program is being compiled ─────────────────────────────── */
_Static_assert(offsetof(struct A, c) == 2 * sizeof(int), "one anonymous level");
_Static_assert(offsetof(struct A, f) == 16 + 2, "a struct inside a union inside the struct");
_Static_assert(offsetof(struct B, in.c) == 2 * sizeof(int), "a later step of a dotted designator");
_Static_assert((size_t)&((struct A *)0)->c == 2 * sizeof(int), "the address spelling");
_Static_assert((size_t)&((struct A *)0)->f == 16 + 2, "the address spelling, two levels");
_Static_assert(offsetof(volatile struct S, b) == sizeof(int), "a qualified container");
_Static_assert(offsetof(const volatile struct S, b) == sizeof(int), "qualified twice");
_Static_assert((size_t)&((volatile struct S *)0)->b == sizeof(int), "a qualified pointee");

static char bound_one[offsetof(struct A, c)];
static char bound_two[offsetof(struct A, f)];
static char bound_dotted[offsetof(struct B, in.c)];
#ifndef NO_SUBSCRIPTED_CONSTANT_DESIGNATOR
/* gcc, clang and mingw-w64 gcc take a SUBSCRIPTED designator as a constant; cl 14.51
 * does not (C2057), promoted member or not — define the macro to build there. */
static char bound_indexed[offsetof(struct T, wide[1].c)];
#endif
static char bound_address[(size_t)&((struct A *)0)->f];
static char bound_qualified[offsetof(struct O, in.b)];
static const size_t in_initializer = offsetof(struct A, f);
static struct A ga;

/* ── the same constant, read by the compiler a second time: the closing brace of
 *    each of these is not reached, and nothing may say it is ───────────────── */
int by_promoted_offset(int x) { if ((size_t)&((struct A *)0)->c == 2 * sizeof(int)) return x + 1; }
int by_promoted_offsetof(int x) { if (offsetof(struct A, f) == 16 + 2) return x + 2; }
int by_qualified_offset(int x) { if ((size_t)&((volatile struct S *)0)->b == sizeof(int)) return x + 3; }

static size_t distance(void const *from, void const *to) {
    return (size_t)((char const *)to - (char const *)from);
}

int main(int argc, char **argv) {
    struct A a;
    struct B b;
    struct T t;
    struct O o;
    size_t const one = (size_t)argc;
    (void)argv;

    /* each constant against the object it describes */
    if (sizeof bound_one != distance(&a, &a.c)) return 1;
    if (sizeof bound_two != distance(&a, &a.f)) return 2;
    if (sizeof bound_dotted != distance(&b, &b.in.c)) return 3;
#ifndef NO_SUBSCRIPTED_CONSTANT_DESIGNATOR
    if (sizeof bound_indexed != distance(&t, &t.wide[1].c)) return 4;
#endif
    if (sizeof bound_address != distance(&a, &a.f)) return 5;
    if (sizeof bound_qualified != distance(&o, (void const *)&o.in.b)) return 6;
    if (in_initializer != distance(&a, &a.f)) return 7;

    /* the same designators where no constant is required */
    if (offsetof(struct A, c) + one != distance(&a, &a.c) + 1) return 8;
    if ((size_t)&((struct A *)0)->f + one != distance(&a, &a.f) + 1) return 9;
    if (offsetof(struct T, wide[1].f) + one != distance(&t, &t.wide[1].f) + 1) return 10;

#if defined(__GNUC__)
    /* the object-size builtin: a promoted member is the subobject it names */
    if (__builtin_object_size(&ga.c, 1) != sizeof(int)) return 11;
    if (__builtin_object_size(&ga.c, 0) != sizeof(struct A) - offsetof(struct A, c)) return 12;
    if (__builtin_object_size(&ga.f, 1) != sizeof(short)) return 13;
#endif

    if (by_promoted_offset(argc) != 2) return 14;
    if (by_promoted_offsetof(argc) != 3) return 15;
    if (by_qualified_offset(argc) != 4) return 16;

    ga.c = 40;
    return ga.c + 2;
}
