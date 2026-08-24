/* c10 D-CSUBSET-STRUCT-MEMBER-DECLARATOR runtime witness.
 *
 * A FUNCTION-POINTER struct member (`int (*op)(int)`) — the SQLite-pervasive
 * shape that, before this cycle, derailed every struct it appeared in (the
 * member declarator was rejected). This exercises the whole chain at runtime:
 *   - the field parses + type-derives as Ptr<FnSig([i32]->i32)> (the struct-
 *     member declarator refactor),
 *   - member ASSIGNMENT stores a function into the field (`a.op = dbl`),
 *   - member-access + INDIRECT CALL invokes through it (`a.op(a.base)`).
 *
 * dbl(5)=10 + inc(20)=21 + 11 = 42.
 *
 * NOTE: brace-init of a fn-ptr member (`struct Ops a = { dbl, 5 };`) is a
 * SEPARATE downstream aggregate-init type-equality concern, deferred to
 * D-CSUBSET-STRUCT-FNPTR-BRACE-INIT (it fails loud at the HIR verifier today);
 * this witness uses member assignment, which the core refactor fully supports. */
static int dbl(int x) { return x + x; }
static int inc(int x) { return x + 1; }

struct Ops { int (*op)(int); int base; };

int main(void) {
    struct Ops a;
    struct Ops b;
    a.op = dbl;  a.base = 5;
    b.op = inc;  b.base = 20;
    return a.op(a.base) + b.op(b.base) + 11;   /* 10 + 21 + 11 = 42 */
}
