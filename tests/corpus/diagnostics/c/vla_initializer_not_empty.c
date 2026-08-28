// D-CSUBSET-VLA-INITIALIZER (C23 6.7.10p4: "An entity of variable length array
// type shall not be initialized except by an empty initializer"). All three
// references refuse every form here (gcc 13.3.0, clang 19.1.1, clang 18.1.3 —
// "variable-sized object may not be initialized"), and before P34 DSS ACCEPTED
// the first one at a compile-time sizeof of 12: the resolver treated a
// present-but-non-constant bound with an initializer as an ABSENT bound and
// re-sized the object from the brace list, discarding the written `argc`.
int main(int argc, char **argv) {
    int a[argc] = {1, 2, 3};
    int b[argc][2] = {{1, 2}};
    const int c[argc] = {0};
    return a[0] + b[0][0] + c[0] + argc - argc;
}
