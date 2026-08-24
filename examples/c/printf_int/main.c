// D-LANG-VARIADIC (step 13.4, 2026-06-02): variadic int arg.
// `printf("answer=%d\n", 42)` passes 2 args against the 1-fixed-
// param variadic FnSig: `fmt` ("answer=%d\n") is the fixed param
// (Ptr<Char>); 42 is the vararg int. The vararg int passes in rsi
// (SysV) / rdx (Win64 slot 1) the same way a non-variadic int
// would — the difference is the cc's `variadicVectorCountReg`
// (rax/al on SysV) gets stamped to 0 before the call because no
// FPR was used in the vararg region.

extern int printf(const char* fmt, ...);

int main() {
    printf("answer=%d\n", 42);
    return 0;
}
