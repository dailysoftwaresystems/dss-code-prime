// C11/C23 6.7.5 (D-CSUBSET-ALIGNAS): a struct MEMBER `alignas(16)` raises the
// struct's alignment AND size END-TO-END (the frontend feeds the interner's
// fieldAligns → computeLayout raises the layout). This RUNS on every target: the
// `_Alignof`/`sizeof`/offsetof are all folded from the raised layout. All values
// are data-model-INDEPENDENT (16-byte alignment + char/int offsets are identical
// under LP64 and LLP64), so the single expected exit code holds across all four
// targets. exit = _Alignof(S)(16) + sizeof(S)(16) + offsetof(T,i)(8) + 2 = 42.

struct S { _Alignas(16) char c; };          // sole member over-aligned → align 16, size 16
struct T { char c; alignas(8) int i; };     // alignas(8) pushes `i` from offset 4 to 8

int main(void) {
    int a = (int)_Alignof(struct S);                       // 16
    int s = (int)sizeof(struct S);                         // 16
    int o = (int)(long long)&((struct T *)0)->i;           // offsetof(T,i) == 8
    return a + s + o + 2;                                  // 16 + 16 + 8 + 2 = 42
}
