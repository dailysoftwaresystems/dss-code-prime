/* A DEEP NEST WITH A LOCAL: the register-pressure half of examples/c/deep_stmt_mir.
 *
 * The same 250-level nest { if (a) { ... { g = 42; } ... } } as its sibling, with g a
 * LOCAL instead of a global. Mem2Reg promotes g into SSA, so every one of the 250
 * joins merges g in a phi: a phi chain as deep as the nest, which out-of-SSA copy
 * placement and register allocation must carry through every level, on every target.
 * The sibling keeps g global precisely so it can stay a pure statement-flattening
 * witness; this example is the shape that sibling used to avoid, because it was once
 * refused (x86 L_VirtualRegInPostRegalloc scratch exhaustion; arm64 frame stores past
 * the unscaled imm9 slot).
 *
 * t() returns 1 and is DECLARED noinline, and that is load-bearing: without it the
 * shipped release pipeline inlines t(), folds all 250 conditions and the phi chain
 * disappears with them (MEASURED 2026-09-19, pe64: a 3072-byte release image against
 * 18944 with noinline) -- the release arm would then witness nothing. MEASURED
 * 2026-09-19 with noinline: compiles and runs to 42 on x86_64:pe64 (native),
 * x86_64:elf64 (WSL) and arm64:elf64 (qemu), at debug and --config=release.
 *
 * MISCOMPILE-SENSITIVE: g starts 0 and only the innermost statement sets it to 42; a
 * phi that took the wrong incoming value at any of the 250 joins, a lost copy, or a
 * spill reloaded from the wrong slot returns 0 (or garbage), not 42. arm64 runs under
 * qemu; macho on the macOS leg. */
__attribute__((noinline)) int t(void) { return 1; }

int main(void) {
    int g = 0;
    int a = t();
    { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { g = 42; } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } }
    return g;
}
