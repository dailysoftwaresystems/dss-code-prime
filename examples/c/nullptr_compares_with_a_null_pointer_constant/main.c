/* P69 L2a (lane lm): `nullptr` compares with a NULL POINTER CONSTANT (C23 6.5.9p2: "one operand has type nullptr_t
   and the other is a null pointer constant") — an integer constant expression of value 0 on either side, under `==`
   and `!=`, folded or not, and `(void *)0`. The lowering reads `nullptr` as the null constant it is, so each
   comparison is the null pointer against itself. gcc 13.3.0 and clang 18.1.3 build this at -std=gnu2x and
   -std=c2x -pedantic-errors and exit 42. Exit 42, or the number of the first check that fails. */
int main(void) {
    if (!(nullptr == 0)) return 1;
    if (!(0 == nullptr)) return 2;
    if (nullptr != 0) return 3;
    if (0 != nullptr) return 4;
    if (!(nullptr == 1 - 1)) return 5;
    if (!(nullptr == (void *)0)) return 6;
    int const both = (nullptr == 0) + (0 != nullptr);
    if (both != 1) return 7;
    return 42;
}
