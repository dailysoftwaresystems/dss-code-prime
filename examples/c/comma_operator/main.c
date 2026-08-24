// FC5 — the comma operator in all three of its roles, each fold-resistant
// (runtime args), pinning the whole source -> binary chain:
//
//   1. SEPARATOR (the dangerous one): `int a = x, b = y;` MUST stay TWO
//      declarators. If the comma-gate broke, it would parse as `int a = (x, y)`
//      (a single declarator initialised to y) and `b` would be undeclared —
//      changing the exit code. This is the multi-site contract's runtime lever.
//   2. SEPARATOR in a for-update: `i = i + 1, j = j - 1` runs both per iteration.
//   3. OPERATOR as a value: `(a = a + 1, a + b)` evaluates the assignment for
//      effect, then yields `a + b` (the right operand).
int compute(int x, int y) {
    int a = x, b = y;                       // (1) two declarators: a=x, b=y
    int t = 0;
    int i, j;
    for (i = 0, j = 10; i < j; i = i + 1, j = j - 1) {  // (2) comma in update
        t = t + 1;                          // i,j converge: 5 iterations
    }
    int v = (a = a + 1, a + b);             // (3) comma value: a++ then a+b
    return v + t;                           // (x+1) + y + 5
}

int main() {
    return compute(18, 18);                 // 19 + 18 + 5 = 42
}
