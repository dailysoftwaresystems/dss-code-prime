/* D-OPT-SQLITE-FPCONV1-RELEASE-FP-MISCOMPILE — the fpconv1 x86_64 release
   miscompile witness (the minimized `dtostr` shape: `double r = arg; int n =
   call; printf("%.*f", n, r)`).

   A `double` promoted by Mem2Reg and SPILLED across an intervening external call
   is passed as a VARIADIC printf argument. On SysV AMD64 the spilled FP vararg
   reaches callconv as a `SpillSlotRef` (c77 D-AS-REGALLOC-DIRECT-ARG-RELOAD), so
   the caller must (a) reload it into its XMM arg register AND (b) count it in the
   AL vector-count (SysV AMD64 §3.5.7). THE BUG: step-13.4's vector-count re-scan
   counted only live-FPR-`Reg` operands and skipped the non-`Reg` SpillSlotRef, so
   AL was stamped 0 -> glibc's variadic prologue `test al,al` skipped saving
   xmm0-7 -> `%f` read the unsaved register-save area -> printed `0.000` instead
   of `3.125`. (The value DID reach xmm0 — the reload was correct all along; AL=0
   meant printf never consulted xmm0.)

   `n = atoi("3")` is an EXTERNAL call that survives inlining/DCE (its result is
   the `%.*f` precision), forcing `r` to live across it; SysV has no callee-saved
   XMM, so under Mem2Reg (release) `r` MUST spill. `--config=debug` (no Mem2Reg)
   keeps `r` in memory and reloads it fresh at the call, so ONLY the RELEASE arm
   witnesses the miscompile — hence the `release` optimizedPipeline below.

   RED-ON-DISABLE (x86_64 elf, release arm): revert lir_callconv.cpp step-13.4 to
   the residency-dependent operand re-scan and this prints `0.000\n` (AL stamped
   0). gcc -std=c17 and the DSS arm64 + debug legs already print `3.125`. */

extern int printf(const char* fmt, ...);
extern int atoi(const char* s);

int main(void) {
    volatile double vr = 3.125;    /* volatile source pins the store (no const-fold) */
    double r = vr;                 /* Mem2Reg-promotable in the release pipeline */
    int n = atoi("3");             /* external call between r's def and use -> r spills */
    printf("%.*f\n", n, r);        /* variadic: GPR vararg n (precision) + spilled FP vararg r */
    return 42;
}
