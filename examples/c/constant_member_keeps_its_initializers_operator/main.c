/* D-C-CONSTANT-AGGREGATE-MEMBER-READ-WITHOUT-ITS-INITIALIZERS-OPERATOR,
 * D-C-GENERIC-SELECTION-BEFORE-THE-EXPRESSION-PASS-SELECTS-DEFAULT,
 * D-HIR-BRACE-PEEL-TAKES-AN-OPERATORS-OPERAND-FOR-THE-ELEMENT,
 * D-HIR-WHOLE-STRUCTURE-BRACE-ELEMENT-IS-PLACED-BY-TYPE-STAMPS — a constant, and
 * a brace element, is read as it was written.
 *
 * The member of a constant aggregate has the value of its initializer (C
 * 6.7.9p11), operator included. The compiler read the member of an object
 * initialized by `-5` as 5 — it walked through the unary operator as through a
 * pair of parentheses — and concluded from that: a condition that is false was
 * taken for true, and the code after its `if` (or after its loop) was deleted.
 * The program then died of an illegal instruction where that code had been.
 *
 * The exit code names the first check that failed:
 *    1..15  A CONDITION on a member initialized through an operator. Each
 *           function must return its argument: the condition is false, and the
 *           statement after it is the one that runs
 *   16..19  the same members at run time (they were always right in the image)
 *   20..23  a generic selection where a constant is wanted before any
 *           expression is typed — an enumerator, an array bound, a bit-field
 *           width: `default` used to be selected for a controlling LITERAL
 *   24..26  an operator's result as the element of a braced character array:
 *           `{ sizeof "abc" }` is one element, not the string
 *   27..30  a whole structure as a brace element, written as an index, a
 *           dereference, a conditional, a comma expression and a call: it
 *           initializes the structure it meets WHOLE (C 6.7.9p13), it is not
 *           taken for the initializer of that structure's first member
 *   42      every one right
 */
typedef unsigned long long u64;
struct S { u64 a; long long b; double d; };
struct T { int x; struct S s; };
union U { long long b; u64 a; };
enum { EN = 5 };
static const int K = 3;
static const struct S h = {.a = 1, .b = 9};

static const struct S g1 = {.b = -5};
static const struct S g2 = {.a = !0, .b = ~0};
static const struct S g3 = {.a = ~0u, .b = -1u, .d = -1.5};
static const struct S g4 = {.a = sizeof K, .b = (-5)};
static const struct S g5 = {.a = sizeof(int), .b = - -3};
static const struct S g7 = {~(unsigned)0x0F, -(unsigned)5, -1U};
static const struct T g8 = {.s = {.b = -EN}};
static const union U g9 = {.b = -5};

/* 1..15 */
static int f1(int x) { if (g1.b != -5) return 0; return x; }
static int f2(int x) { if (g2.a != 1) return 0; return x; }
static int f3(int x) { if (g2.b != -1) return 0; return x; }
static int f4(int x) { if (g3.a != 0xFFFFFFFFull) return 0; return x; }
static int f5(int x) { if (g3.d != -1.5) return 0; return x; }
static int f6(int x) { if (g4.a != sizeof(int)) return 0; return x; }
static int f7(int x) { const struct S a = {.a = !K, .b = -K}; if (a.b != -3) return 0; return x; }
static int f8(int x) { const struct S a = {.a = ~h.a, .b = -h.b}; if (a.b != -9) return 0; return x; }
static int f9(int x) { if (g1.b == -5) { } else return 0; return x; }
static int f10(int x) { int n = 0; while (g1.b != -5) { if (++n > 3) return 0; } return x; }
static int f11(int x) { int n = 0; for (; g8.s.b != -5;) { if (++n > 3) return 0; } return x; }
static int f12(int x) { int n = 0; do { if (++n > 3) return 0; } while (g9.b != -5); return x; }
static int f13(int x) { const struct S a = {.b = -5}; if (a.b != -5) return 0; return x; }
static int f14(int x) { if ((struct S){.b = -5}.b != -5) return 0; return x; }
static int f15(int x) {
    if (g7.a != 0xFFFFFFF0ull) return 0;
    if (g7.b != 4294967291ll) return 0;
    if (g7.d != 4294967295.0) return 0;
    return x;
}

/* 20..23 */
enum { SELECTED = _Generic(0, int: 42, default: 9), BY_DOUBLE = _Generic(1.5, double: 42, default: 9) };
static char bound[_Generic(0, int: 3, default: 7)];
struct B { unsigned f : _Generic(0, int: 3, default: 9); };

/* 24..26 */
static char one[] = { sizeof "abc" };

/* 27..30 */
static struct S table[2] = { {.a = 1, .b = 2}, {.a = 3, .b = 4} };
static struct S pick(void) { return table[1]; }

static volatile int source = 7;
static const struct S *volatile seen;

int main(void) {
    int x = source;
    struct B b = { 0 };
    char first[] = { *"abc" };
    char two[] = { *"abc", 'z' };
    if (f1(x) != 7) return 1;
    if (f2(x) != 7) return 2;
    if (f3(x) != 7) return 3;
    if (f4(x) != 7) return 4;
    if (f5(x) != 7) return 5;
    if (f6(x) != 7) return 6;
    if (f7(x) != 7) return 7;
    if (f8(x) != 7) return 8;
    if (f9(x) != 7) return 9;
    if (f10(x) != 7) return 10;
    if (f11(x) != 7) return 11;
    if (f12(x) != 7) return 12;
    if (f13(x) != 7) return 13;
    if (f14(x) != 7) return 14;
    if (f15(x) != 7) return 15;
    seen = &g3;
    if (seen->a != 0xFFFFFFFFull || seen->b != 4294967295ll || seen->d != -1.5) return 16;
    seen = &g4;
    if (seen->a != sizeof(int) || seen->b != -5) return 17;
    seen = &g5;
    if (seen->a != sizeof(int) || seen->b != 3) return 18;
    seen = &g8.s;
    if (seen->b != -5) return 19;
    if (SELECTED != 42) return 20;
    if (BY_DOUBLE != 42) return 21;
    if (sizeof bound != 3) return 22;
    b.f = ~0u;
    if (b.f != 7u) return 23;
    if (sizeof one != 1 || one[0] != 4) return 24;
    if (sizeof first != 1 || first[0] != 'a') return 25;
    if (sizeof two != 2 || two[0] != 'a' || two[1] != 'z') return 26;
    {
        int zero = x - 7;
        struct S *pp = &table[0];
        struct S byIndex[] = { table[1], table[zero] };
        struct S byOthers[] = { *pp, *(pp + 1), (zero ? table[0] : table[1]), (zero, table[0]), pick() };
        struct T member = { 5, table[1] };
        struct T designated = { .s = *pp, .x = 6 };
        if (sizeof byIndex != 2 * sizeof(struct S) || byIndex[0].b != 4 || byIndex[1].b != 2) return 27;
        if (sizeof byOthers != 5 * sizeof(struct S) || byOthers[0].b != 2 || byOthers[1].b != 4
            || byOthers[2].b != 4 || byOthers[3].b != 2 || byOthers[4].b != 4) return 28;
        if (member.x != 5 || member.s.b != 4 || member.s.a != 3) return 29;
        if (designated.x != 6 || designated.s.b != 2 || designated.s.a != 1) return 30;
    }
    return 42;
}
