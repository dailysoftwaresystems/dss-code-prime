// Regression corpus for D-HIR-LOOP-BODY-ONLY-RETURN-DOUBLE-ATTACH,
// `for(;;)` variant: a clause-less `for` is the second provably-
// infinite loop shape whose only exit is a `return`. Same crash path
// as the `while(1)` case — the implicit `return 0` appended after a
// non-terminating body used to double-attach the body's children.
// The dead `return 0` is pruned in MIR; the observable exit is 9.
int main() {
    for (;;) {
        return 9;
    }
}
