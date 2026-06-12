// FC4 c1 stage 2b — typedef NAMES in DECLARATION position (closes the
// declaration-statement scope of D-CSUBSET-TYPEDEF-DECLARATION-POSITION;
// typedef_cast covers the CAST position — a distinct gap).
//
//   * `myint x = 40;`  — bare-typedef-name head as a LOCAL declaration
//     statement: the identVarDecl speculative path (binder sketch knows
//     `myint` is a Type -> commit). A triage regression reparses it as
//     the expression `myint * ...` and fails the compile loudly.
//   * `typedef struct P { int a; } MyP;` — typedef-position struct BODY
//     (structSpecifierBody) minting the composite + the alias.
//   * `MyP *pp = 0;` — the typedef'd STRUCT type used in DECLARATION
//     position via a POINTER declarator (no struct-VALUE runtime
//     machinery needed); 0 is the null-pointer constant.
//
// Exit arithmetic: x + (pp == 0 ? 2 : 9) = 40 + 2 = 42. A broken
// null-init (pp != 0) -> 49; delta 7 != 0 mod 256.
typedef int myint;
typedef struct P { int a; } MyP;

int main() {
    myint x = 40;
    MyP *pp = 0;
    return x + (pp == 0 ? 2 : 9);
}
