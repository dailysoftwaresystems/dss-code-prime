// C23 §6.3.2.3 (D-CSUBSET-NULLPTR): `nullptr` (nullptr_t) is NOT assignable to an
// integer — it converts only to pointer types and to bool. `int x = nullptr;` is a
// constraint violation → S_TypeMismatch at the `nullptr` initializer (NOT silently
// lowered to `int x = 0;`). The isAssignable NullptrT arm admits pointer targets
// only (nullptr→bool is deferred: D-CSUBSET-NULLPTR-BOOL-CONVERSION).
int main(void) {
    int x = nullptr;
    return x;
}
