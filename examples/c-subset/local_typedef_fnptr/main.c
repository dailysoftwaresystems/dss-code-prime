/* c30 RUNTIME WITNESS (D-CSUBSET-LOCAL-TYPEDEF): a BLOCK-SCOPED typedef as a
 * STATEMENT inside a function — the sqlite3.c:187603 shape
 * (`typedef void(*LOGFUNC_t)(void*,int,const char*);` mid-function, then a local
 * variable of the typedef'd type assigned + used). The grammar now admits
 * `typedefDecl` as a block item; the alias binds in the ENCLOSING BLOCK scope;
 * a later `FN_t f = ...;` resolves it and a CALL THROUGH the fn-ptr drives the
 * exit code (so the fn-ptr value round-trips through a frame slot + an indirect
 * call — a mis-resolved alias type or a wrong fn-ptr ABI derails the exit).
 *
 *   compute():  typedef int (*FN_t)(int);   (block-scoped fn-ptr typedef)
 *               FN_t f = times3; FN_t g = plus1;
 *               f(13)=39 + g(2)=3  → 42
 *
 * RED-ON-DISABLE: revert the `statement` alt tweak (drop `typedefDecl`) → the
 * block `typedef` is a parse error (P0009) and the corpus never builds. The
 * block-scope NON-LEAK + SHADOW are pinned at the unit tier (a parse-oracle leak
 * is invisible to a green corpus). Release pipeline + all 4 targets. */

static int times3(int x) { return x * 3; }
static int plus1(int x)  { return x + 1; }

int compute(void) {
    /* The c30 construct: a block-scoped fn-ptr typedef, used via call-through. */
    typedef int (*FN_t)(int);
    FN_t f = times3;
    FN_t g = plus1;
    return f(13) + g(2);
}

int main(void) {
    return compute();
}
