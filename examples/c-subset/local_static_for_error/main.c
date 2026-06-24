// D-CSUBSET-LOCAL-STATIC (MF-2): a `static` storage-class specifier in a
// for-statement init-declaration is a C 6.8.5p3 constraint violation ("the
// declaration part of a for statement shall only declare identifiers for
// objects having storage class auto or register"). It PARSES (the shared
// localDeclSpecifiers prefix admits `static`) but must FAIL LOUD —
// S_StaticStorageInForInit, positioned at the `static` token — never silently
// lower as an automatic loop variable.
int main(void) {
    for (static int i = 0; i < 3; i = i + 1) {
    }
    return 0;
}
