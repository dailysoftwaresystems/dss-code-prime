/* Cluster F item F1 (C 6.5.2.4 / 6.5.3.1) — ++/-- end-to-end RUNTIME witness
 * across every target. Exercises the four behaviors F1 adds/fixes, each wired so
 * a regression in ANY of them flips the exit code away from 0:
 *
 *   (1) PRE vs POST are DISTINCT (integer):
 *         x = 5; a = x++;  -> a == 5 (OLD), x == 6
 *                b = ++x;  -> b == 7 (NEW), x == 7
 *       If prefix wrongly yielded the OLD value (or postfix the NEW), a/b flip.
 *
 *   (2) POINTER step is sizeof-SCALED, not 1 byte (the silent-miscompile FIX):
 *         int arr[3] = {10, 20, 30}; int* p = arr; p++;  -> *p == 20
 *       A 1-byte step (the old BinaryOp(Add,ptr,1)) reads inside arr[0], not
 *       arr[1] — *p would NOT be 20.
 *
 *   (3) PREFIX pointer step is ALSO scaled (the other MF-1 site):
 *         int* q = arr; ++q;  -> *q == 20
 *
 *   (4) SINGLE-EVALUATION of a complex lvalue: `acc[side()]++` calls side()
 *       EXACTLY once (the lvalue's address is bound to a temp ONCE), so the
 *       global `calls` counter == 1 and acc[0] is incremented once (1 -> 2).
 *
 * Each check contributes 0 to `bad` when correct; main returns `bad` (0 on a
 * fully-correct compiler). The optimizedPipelines `release` arm proves the flow
 * survives the shipped optimizer; arm64 runs under qemu, macho on macOS. */

int calls = 0;
int acc[2] = {1, 7};

int side(void) {       /* must be evaluated EXACTLY once by acc[side()]++ */
    calls = calls + 1;
    return 0;          /* index 0 */
}

int main(void) {
    int bad = 0;

    /* (1) pre vs post distinct (integer) */
    int x = 5;
    int a = x++;                 /* a = 5 (old), x = 6 */
    int b = ++x;                 /* b = 7 (new), x = 7 */
    if (a != 5) bad = bad + 1;
    if (b != 7) bad = bad + 2;
    if (x != 7) bad = bad + 4;

    /* (2) pointer postfix step is sizeof(int)-scaled */
    int arr[3];
    arr[0] = 10; arr[1] = 20; arr[2] = 30;
    int* p = arr;
    p++;                         /* p -> &arr[1] (scaled by 4 bytes, not 1) */
    if (*p != 20) bad = bad + 8;

    /* (3) pointer prefix step is ALSO scaled */
    int* q = arr;
    ++q;                         /* q -> &arr[1] */
    if (*q != 20) bad = bad + 16;

    /* (4) single-evaluation of a complex lvalue: side() called once */
    acc[side()]++;               /* acc[0]: 1 -> 2 ; side() evaluated ONCE */
    if (calls != 1) bad = bad + 32;
    if (acc[0] != 2) bad = bad + 64;

    return bad;                  /* 0 iff every F1 behavior is correct */
}
