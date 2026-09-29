// [[D-C-TAG-DEFINED-IN-A-PARAMETER-LIST-REFUSED]] — a parameter may DEFINE its
// struct, union or enum type.
//
// C 6.7.3.4 declares a tag wherever its specifier appears, and C 6.2.1p4 scopes
// one written in a parameter list: visible to the rest of the list and, in a
// definition, to the body; invisible after the function. ✔MEASURED 2026-09-23:
// gcc 13.3.0 and clang 18.1.3 (-std=c2x and -std=c17), MinGW gcc and MSVC 19.51
// build this file and RUN it to 42; gcc and clang warn that each such tag is not
// visible outside. DSS refused every one of these shapes with P_UnexpectedToken.
//
// Runtime witness: each function uses the type its own parameter list defines,
// and main subtracts what each must return. 42 means every one of them computed
// with the type its list gave it.

// a struct defined in a definition's list, named again in the body
static int sum_pair(struct Pair { int a; int b; } *p) {
    struct Pair local = { 40, 2 };
    if (p == 0) p = &local;
    return p->a + p->b;                       // 42
}

// a union
static int union_word(union Word { int i; char c; } *w) {
    union Word local;
    local.i = 7;
    if (w == 0) w = &local;
    return w->i;                              // 7
}

// an enum: its enumerators are in scope in the body
static int enum_step(enum Step { STEP_A = 40, STEP_B } e) {
    return e + (STEP_B - STEP_A);             // e + 1
}

// an anonymous struct: only the list can name it
static int anon_sum(struct { int a; int b; } *p) {
    return p ? p->a + p->b : 5;               // 5
}

// a tag nested in a tag the list defines: both reach the body
static int nested_inner(struct Outer { struct Inner { int x; } in; } *o) {
    struct Inner q = { 3 };
    (void)o;
    return q.x;                               // 3
}

// a later parameter names the tag an earlier one defined
static int same_tag_later(struct Cell { int v; } *p, struct Cell *q) {
    struct Cell t = { 11 };
    if (p == 0) p = &t;
    if (q == 0) q = p;
    return q->v;                              // 11
}

// a tag defined in a function-POINTER parameter's own list: prototype scope
static int apply(int (*cb)(struct Probe { int b; } *t)) {
    return cb ? 0 : 1;                        // 1
}

// a tag defined in a type name inside a parameter's declarator
static int sized(int a[sizeof(struct Ten { char x[10]; })]) {
    (void)a;
    return (int)sizeof(struct Ten);           // 10
}

// a prototype whose list defines a tag; it only has to declare
int declared_only(struct Only { int b; } *t);

int main(void) {
    int block_proto(struct Local { int z; } *t);   // a block-scope prototype
    int arr[10] = { 0 };
    int total = 0;
    total += sum_pair(0) - 42;
    total += union_word(0) - 7;
    total += enum_step(40) - 41;
    total += anon_sum(0) - 5;
    total += nested_inner(0) - 3;
    total += same_tag_later(0, 0) - 11;
    total += apply(0) - 1;
    total += sized(arr) - 10;
    return 42 + total;
}
