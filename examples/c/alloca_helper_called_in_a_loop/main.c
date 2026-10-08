/* P69 (lane `cs`, review M2) — D-CSUBSET-GNUC-PREDEFINE-SELECTS-UNIMPLEMENTED-BUILTIN: a small helper
 * that allocates with `__builtin_alloca`, called in a loop. Each block is the HELPER's frame's, so
 * each call's block is gone when the call returns. An optimizer that splices the helper into its
 * caller makes the block the CALLER's — live until the caller returns — and 10000 calls of 4 KiB
 * then hold 40 MiB of stack at once. So a body whose alloca is runtime-sized is never inlined:
 * the call stays, and the loop runs in constant stack. Exit 42 when the loop finishes with every
 * call's answer right; a stack overflow ends the process instead.
 * Under a compiler defining no `__GNUC__` (MSVC 19.51) the file is a bare `return 42`. */
#if defined(__GNUC__)
static int helper(int n) {
    char *p = __builtin_alloca(n);
    p[0] = 1;
    p[n - 1] = 2;
    return p[0] + p[n - 1];
}

int main(int argc, char **argv) {
    (void)argv;
    int n = argc * 4096;   /* 4096 under every runner: the size is not a constant to the optimizer */
    long s = 0;
    for (long i = 0; i < 10000; ++i) s += helper(n);
    return s == 30000 ? 42 : 1;
}
#else
int main(void) { return 42; }
#endif
