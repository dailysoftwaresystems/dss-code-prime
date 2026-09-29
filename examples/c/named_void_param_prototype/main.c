// P68 round 8 (lane `ht`, part 2) — a NAMED `void` parameter in a declaration that
// is not a definition, with gcc's measured meaning: a parameter of an incomplete
// type at which a call's arguments END.
//
// gcc 13.3.0 and mingw-w64 13.2.0 accept these prototypes (clang 18.1.3 and MSVC
// 19.51 refuse every one), and the program gcc builds RUNS: each call reaches the
// definition in helper.c, which declares the function the ordinary way. DSS refused
// all of them (S_InvalidVoidParam) until this cycle. Now it keeps the parameter in
// the signature — a type distinct from `void(void)`, as gcc's is — binds a call's
// arguments to the parameters BEFORE the void, and its cross-TU merge binds each
// extern to its definition.
//
// Value-divergent: seen = 1 (f) + 1 (f through the pointer) + 40 (g) = 42. A call
// that lost its argument, or never reached helper.c, cannot produce 42.
//
// RED-ON-DISABLE: make `TypeInterner::fnArgumentParams` return the whole declared
// list -> the semantic call check refuses `f()` (S_ArgCountMismatch: 0 arguments for
// 1 parameter) and this example does not compile.

void f(void v);        /* one parameter, of type void: a call's arguments end at it */
void g(int a, void v); /* one argument, then the void */
extern int seen;

int main(void) {
    void (*p)(void w) = f; /* the same type: a parameter's name is not part of it */
    f();
    p();
    g(40);
    return seen;
}
