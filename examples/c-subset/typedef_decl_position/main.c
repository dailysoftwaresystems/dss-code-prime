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
//   * `struct P sp;` — the TAG `P` used DIRECTLY in type position (NOT
//     via the alias). C 6.2.3 tag-namespace regression guard: with a
//     SEPARATE tag namespace, `struct P` must resolve the tag while the
//     bare alias `MyP` resolves Ordinary — both reaching the SAME struct
//     type. (`P` != `MyP` here, so this also confirms the distinct-tag
//     path the tag namespace must keep working.)
//
// Exit arithmetic: x + sp.a + (pp == 0 ? 2 : 9) = 38 + 2 + 2 = 42. A
// broken null-init (pp != 0) -> 49; delta 7 != 0 mod 256.
typedef int myint;
typedef struct P { int a; } MyP;

int main() {
    myint x = 38;
    struct P sp;
    sp.a = 2;
    MyP *pp = 0;
    return x + sp.a + (pp == 0 ? 2 : 9);
}
