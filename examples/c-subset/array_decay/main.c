/* D-LK4-RODATA-PRODUCER-LOCAL-ARRAY-DECAY + D-LK4-RODATA-PRODUCER-NONSTRING-
 * GLOBAL-ARRAY-DECAY (FC8) end-to-end RUNTIME witness across all targets.
 * Non-string array -> pointer DECAY (C 6.3.2.1): an array used where a pointer
 * is expected decays to the address of its first element. Exercised:
 *   - LOCAL array decay: `bump(a)` / `sum3(a)` pass `int a[3]` (an alloca) as
 *     `int*` -> the alloca's base address;
 *   - GLOBAL array decay: `sum3(g)` passes the file-scope `int g[3]` (its bytes
 *     emitted to rodata by the aggregate-global producer) -> its GlobalAddr;
 *   - WRITE-THROUGH: `bump(a)` writes `a[0]` through the decayed pointer, proving
 *     it aliases the real array storage (not a copy).
 * a = {3,4,1}; bump(a) -> a[0]=4 -> a={4,4,1}; sum3(a)=9; sum3(g)=10+20+1=31.
 * return 9 + 31 + 2 = 42. A decay that produced a wrong address / a copy / a
 * fail-loud flips the exit. Operands are runtime (non-inlined calls), so no pass
 * folds the decay away. arm64 runs under qemu; macho on the macos-latest leg. */
int g[3] = {10, 20, 1};   /* a non-string array GLOBAL (rodata) */

int sum3(int* p) { return p[0] + p[1] + p[2]; }   /* reads through a decayed ptr */
void bump(int* p) { p[0] = p[0] + 1; }            /* WRITES through a decayed ptr */

int main(void) {
    int a[3];
    a[0] = 3; a[1] = 4; a[2] = 1;
    bump(a);                       /* local decay + write-through: a[0] -> 4 */
    return sum3(a) + sum3(g) + 2;  /* 9 + 31 + 2 = 42 */
}
