/* c32 D-CSUBSET-FNPTR-PARAM-SCOPE runtime witness.
 *
 * A struct of FUNCTION-POINTER members whose param names are SHARED across the
 * sibling declarators (`int (*lo)(int v); int (*hi)(int v);`) — the
 * sqlite3_io_methods shape (`int (*xRead)(... int iAmt ...); int (*xWrite)(...
 * int iAmt ...);`). Before this cycle each fn-ptr member's param NAMES bound
 * into the enclosing STRUCT scope, so the second `v` collided with the first
 * (error[S0002] S_RedeclaredSymbol) and the struct was rejected. With a
 * per-declarator function-prototype scope (C 6.2.1p4) the two `v` params live in
 * DISTINCT throwaway scopes — no collision, no leak — while the members `lo`/`hi`
 * still type-derive as Ptr<FnSig([i32]->i32)>, member-assign, and CALL THROUGH.
 *
 * Exit arithmetic: lo(2)=2 (identity) ; hi(40)=40 (identity) ; 2 + 40 = 42.
 * The two fn-ptr members are assigned distinct functions and dispatched through
 * member access + indirect call, so a clobbered/leaked binding shifts the exit
 * off 42.
 *
 * RED-ON-DISABLE: revert the prototype-scope open (semantic_analyzer.cpp
 * nodeOpensChildScope / isPrototypeParamScopeNode) and this source fails to
 * compile — the second `v` collides (S_RedeclaredSymbol) and the struct is
 * rejected. */
static int ident_lo(int v) { return v; }
static int ident_hi(int v) { return v; }

struct Dispatch {
    int (*lo)(int v);
    int (*hi)(int v);
};

int main(void) {
    struct Dispatch d;
    d.lo = ident_lo;
    d.hi = ident_hi;
    return d.lo(2) + d.hi(40);   /* 2 + 40 = 42 */
}
