// D-CSUBSET-CONSTEXPR: a `constexpr` object of VARIABLY MODIFIED type has no
// compile-time size, so no element walk can make it a constant object — gcc
// ("'constexpr' object has variably modified type") and clang ("constexpr
// variable cannot have type 'const int[argc]'") both refuse it.
//
// ⚠ THE FIXTURE MOVED IN P34. It used to read `constexpr int a[argc] = {1,2,3};`,
// which only reached the constexpr validator because the declarator resolver
// DROPPED the written `argc` bound and re-sized the object from the brace list.
// That dropped bound was itself the defect D-CSUBSET-VLA-INITIALIZER closed: the
// non-empty form is now a constraint violation refused one tier earlier (C23
// 6.7.10p4; its own fixture is `vla_initializer_not_empty.c`). The EMPTY
// initializer is the one form 6.7.10p4 permits, so it really is a legal VLA and
// really does reach this validator — which is what keeps `constexpr` the thing
// this file pins.
int main(int argc, char **argv) {
    constexpr int a[argc] = {};
    return a[0];
}
