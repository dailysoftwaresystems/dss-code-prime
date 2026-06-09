// D-AS3-BLOCK-REL-IMM19/26 (ARM64 conditional control-flow) runtime corpus.
// The cross-target end-to-end proof that the AArch64 cmp / setcc / jcc / jmp
// back-edge + alloca path EXECUTES — not just assembles. A count-up while loop
// runs the loop body 42 times and returns the counter as the process exit code.
//
// Exercises at RUNTIME on arm64 (verified on the native-arm64 CI leg under
// qemu-aarch64; SkippedCrossHost on a non-arm64 host with no emulator):
//   * the `int n` local  -> alloca -> frame-relative lea (ADD Xd, sp, #imm12)
//   * `n < 42`           -> cmp (SUBS XZR) feeding the loop branch
//   * the loop branch    -> jcc (B.cond, Imm19) + jmp (B, Imm26) back-edge,
//                           with the block-relative resolver filling the
//                           signed, scaled, instruction-PC-relative offsets.
// Note: lowerCondBr FUSES the `n < 42` ICmp into the cmp+B.cond pair, so the
// separately-lowered setcc(CSET)/zext are EMITTED but dead here (no consumer;
// the debug pipeline has no DCE) — they execute without faulting but their
// result is unused, so this corpus does NOT guard CSET/zext CORRECTNESS. That
// is pinned host-independently by the asm byte-tests (CsetEncodesInvertedCond-
// AtBits12, ZextEncodesOrrW). The corpus's live arm64 control-flow proof is
// cmp + jcc + jmp(back-edge) + the alloca frame-lea.
//
// The x86_64 arm runs the SAME source on the x86 control-flow path, so the
// corpus is a genuine cross-target differential: both legs must exit 42.
int main() {
    int n = 0;
    while (n < 42) {
        n = n + 1;
    }
    return n;
}
