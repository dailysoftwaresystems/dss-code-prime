// P68 round 13 (lane `cs`, the static-initializer item) — C 6.7.9p4: every expression in
// an initializer for an object of static or thread storage duration shall be a constant
// expression or a string literal. Each marked initializer provably is not, every reference
// refuses each, and so does DSS, at the construct.
int y = 42;
int x = y;                                   // a non-const object's value
int a[2] = { 1, y };                         // ... as an aggregate element

int main(void) {
    int l = 42;
    static int *p = &l;                      // a local's address (it used to abort DSS)
    return x + a[1] + *p;
}
