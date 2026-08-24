// D-CSUBSET-ANON-TYPEDECL-TYPE-FALLBACK: a file-scope ANONYMOUS enum (tagless
// `enum { … }`) whose enumerator is referenced in a file-scope constant-expression
// (the array dimension `arr[V]`). Before the fix this failed to compile with
// error[H0001] (H_TypeUnresolved) on the anonymous enum's HIR TypeDecl — the CST→HIR
// lowering read the type off the name-child (the enum BODY for an anon enum, which
// carries no symbol) instead of off the specifier node the analyzer stamped. The
// NAMED equivalent (`enum E { … }`) was always clean. `sizeof(arr)` == V*sizeof(int)
// == 16*4 == 64 proves the enumerator value flowed into the array dimension.
// Red-on-disable: drop the `model.typeAt(node)` fallback in lowerTypeDecl and this
// no longer compiles (H0001). One exit code across all four targets.
enum { V = 16 };
int arr[V];
int main(void) {
    return (int)sizeof(arr);
}
