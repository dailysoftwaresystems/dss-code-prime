/* A CALL TO A FUNCTION ONLY ANOTHER BLOCK OF THE UNIT DECLARED TAKES THAT DECLARATION, WARNED (P69 round 1, lane cs,
 * fold f26b). A block-scope function declaration has its block's scope (C 6.2.1p4); outside the block a call that names
 * nothing visible is resolved by cl and clang to it (clang: "use of out-of-scope declaration"), its type included --
 * so `g_double(40)` converts its argument to double. other.c defines each function. Every return before 42 names the
 * first fact that failed. */

static void declare_them(void) {
    int g_int(int);
    double g_double(double);
}

static int after_its_block_ended(void) {
    { int g_ended(int); }
    return g_ended(40);                      /* the block has ended: out of scope, taken, warned */
}

int main(void) {
    declare_them();
    if (g_int(40) + 2 != 42) return 1;           /* declared only in another function's block */
    if ((int)g_double(40) + 2 != 42) return 2;   /* its declared type: 40 is passed as 40.0 */
    if (after_its_block_ended() + 2 != 42) return 3;
    return 42;
}
