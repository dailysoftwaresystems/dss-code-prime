/* c36 (D-CSUBSET-MUTABLE-POINTER-TO-CONST): a `const int *p` is a MUTABLE
 * pointer to const int — the canonical sqlite `zFormat += 4` / `zDate++`
 * shape (const qualifies the POINTEE, not the pointer OBJECT; C 6.7.3).
 * `sum_n` walks an array by ADVANCING the pointer (`p++`), which c36 makes
 * legal. Pre-c36 the coarse whole-decl const scan marked `p` const, so `p++`
 * raised a spurious S_ConstViolation and this corpus would NOT EVEN COMPILE —
 * that is the red-on-disable witness (compile is the pin; exit 42 confirms the
 * walk derefs the right slots). The pointee stays const (never written).
 *
 * Uses int (32-bit) arithmetic/compares only (the deferred sub-32-bit ALU gap,
 * D-CSUBSET-32BIT-ALU-FORMS, blocks char compares) and `p++` (an int* steps
 * sizeof(int), F1-correct — NOT the binary `p + n` whose element stride is
 * separately flagged). 10 + 20 + 12 = 42. */
static int sum_n(const int *p, int count) {
    int n = 0;
    while (count > 0) {     /* 32-bit int compare */
        n = n + *p;         /* int load + int add */
        p++;                /* advance the MUTABLE pointer — the c36 fix */
        count = count - 1;
    }
    return n;
}

int main(void) {
    int a[3];
    a[0] = 10;
    a[1] = 20;
    a[2] = 12;
    const int *p = a;       /* mutable pointer to const int (pointee const) */
    return sum_n(p, 3);     /* 10 + 20 + 12 = 42 */
}
