/* c35 (D-CSUBSET-FORWARD-STRUCT-DECLARATION): an OPAQUE handle — `struct Stmt`
 * is FORWARD-DECLARED and NEVER defined (the sqlite3_stmt pattern: a public
 * incomplete type used only behind a pointer). The real object is a DIFFERENT,
 * fully-defined `struct Vdbe`, reached through the handle via `void *` (the
 * Vdbe-behind-sqlite3_stmt* idiom). The opaque `struct Stmt *` is passed BY
 * POINTER through a NON-inlined call (`step` → `step_impl`); only inside the
 * implementation is it cast (through `void *`) back to the concrete `struct
 * Vdbe *` whose fields are then read. This exercises the c35 contract: a
 * `Ptr<incomplete>` is a sizeable, passable value (a plain pointer), while the
 * incomplete `struct Stmt` itself is NEVER sized (no value, no member, no
 * sizeof of it anywhere — those would fail loud).
 *
 * Sum = 30 + 12 = 42. RED-ON-DISABLE: without the forward-mint the opaque
 * `struct Stmt *` would not even type-check (S_UnknownType); a wrong `struct
 * Vdbe` field offset derails the 42. The cross-`void *` round-trip + the
 * non-inlined call keep the handle from being folded away. */
struct Stmt;                 /* opaque — forward-declared, never defined */

struct Vdbe { int a; int b; };   /* the concrete object behind the handle */

/* Concrete implementation: recover the real object from the opaque handle. */
static int step_impl(void *raw) {
    struct Vdbe *v = (struct Vdbe *)raw;
    return v->a + v->b;
}

/* Public entry: takes the OPAQUE handle by pointer, forwards it as void*. */
int step(struct Stmt *s) {
    return step_impl((void *)s);
}

int main(void) {
    struct Vdbe real;
    real.a = 30;
    real.b = 12;
    /* Hand the concrete object out AS the opaque handle (Vdbe → sqlite3_stmt*). */
    struct Stmt *handle = (struct Stmt *)(void *)&real;
    return step(handle);
}
