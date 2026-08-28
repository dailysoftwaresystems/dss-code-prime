// D-CSUBSET-FN-TYPE-TYPEDEF-PAREN-NAME (TF-C64): a FUNCTION-TYPE typedef via a
// PARENTHESIZED NAME — `typedef R (Name)(args);` — aliases the FUNCTION type
// itself (no `*`, so NOT a function pointer). This is the shape real tcl.h uses
// pervasively: `typedef int (Tcl_AppInitProc)(Tcl_Interp *)`, `typedef void
// (Tcl_CmdDeleteProc)(ClientData)`, ~90 of them. `BinOp` below is used exactly
// as tcl.h uses those names — as the pointee type of a passed-in callback
// (`BinOp *op`), the whole reason a function-TYPE (not pointer) typedef exists.
typedef int (BinOp)(int, int);

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }

// `op` is a pointer to the fn-TYPE typedef, taken as a PARAMETER — the call
// through it is a genuine indirect call (the callee is not known at this site).
static int apply(BinOp *op, int a, int b) { return op(a, b); }

int main(void) {
    int r = apply(add, 40, 5);   // 45
    r = apply(sub, r, 3);        // 42
    return r;
}
