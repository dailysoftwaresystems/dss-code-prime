// FC4 c1 stage 2b — multi-declarator declaration statements (C 6.7.6):
// ONE declaration, MANY declarators; the star binds PER-DECLARATOR.
//
//   int *p, q;   ->  p : Ptr<I32>,  q : I32   (NOT both pointers)
//
// Discriminators:
//   * A regression that typed q as Ptr<I32> FAILS THIS COMPILE: `q = 40;`
//     would assign a non-zero int into a pointer (pointerConversions
//     admit only the literal-0 null-pointer constant), so compile-success
//     is itself a witness; the exit code then witnesses the VALUE flow
//     through *p.
//   * for-init multi-declarator: `int i = 0, j = 3` — BOTH loop-scoped,
//     j carries the bound (a mis-minted j would either fail compile or
//     change the trip count: acc 3 -> exit != 42, delta < 256).
//   * mixed-form list `int x = 30, *px = &x, y[2];` — initializer +
//     pointer-init + array declarator in ONE list; *px witnesses the
//     mid-list pointer at runtime (y's Array<2,I32> typing is the
//     parse+type witness; it stays unused).
//
// Exit arithmetic: *p + acc + *px - 31 = 40 + 3 + 30 - 31 = 42.
// Every wrong-path delta (acc=0 -> 39; *px broken -> compile fail or
// 12) is != 0 mod 256.
int main() {
    int *p, q;
    q = 40;
    p = &q;
    int acc = 0;
    for (int i = 0, j = 3; i < j; i++) {
        acc = acc + 1;
    }
    int x = 30, *px = &x, y[2];
    return *p + acc + *px - 31;
}
