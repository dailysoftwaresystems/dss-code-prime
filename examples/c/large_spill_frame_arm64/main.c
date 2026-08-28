/* D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12 (load/store-displacement half) RUN
 * witness: the AArch64 backend's SCALED-imm12 unsigned-offset frame load/store
 * form (LDR/STR [sp,#imm], reach 0..4095*size), selected at the emitFrameLoad/
 * emitFrameStore CHOKEPOINT (selectFrameMemOp in lir_callconv.cpp) whenever a
 * frame offset exceeds the unscaled imm9 ±256 reach.
 *
 * `g` takes 30 fixed int params: AAPCS64 passes the first 8 in x0..x7 and the
 * rest (p08..p29) on the INCOMING STACK at `[sp + totalFrameSize + (i-8)*8]`.
 * `main` CALLS g with 30 args, so the 9th+ args are STORED into the OUTGOING
 * stack-arg area; under the un-optimized pipeline both sides push a frame
 * offset PAST imm9 (offsets 256 / 264 observed). Concretely this exercises:
 *   - the OUTGOING stack-arg STORE (main writing args 9..30 → store_u), and
 *   - the INCOMING stack-arg LOAD + spill reload in g (reading p28/p29 → load_u),
 *   - plus the caller's spill store/reload of the live arg values (store_u/load_u).
 * Every one of those is a frame load/store routed through the chokepoint; an
 * offset beyond 255 takes the scaled `load_u`/`store_u` (LDR/STR, 0xF9 mode).
 *
 * RED-ON-DISABLE: revert the chokepoint swap (keep the unscaled `load`/`store`)
 * → the arm64 compile FAILS LOUD `A_ImmediateOperandOutOfRange` at offsets 256
 * and 264 (store AND load) → A_FunctionEncodeAborted → this example cannot build.
 *
 * MISCOMPILE-SENSITIVE EXIT 42: g returns p28 + p29 + p08 (three params at the
 * stack boundary). main passes p08=1, p28=1, p29=40, everything else 0, so the
 * sum is exactly 42. A wrong frame offset (mis-scaled imm12, or the wrong
 * load/store mode) would read a neighbouring slot and return a different value.
 *
 * arm64-only (the witness target): the un-optimized x86_64 caller exhausts its
 * 16-GPR scratch pool materializing 30 args before the call, a SEPARATE
 * pre-existing regalloc limit (L_VirtualRegInPostRegalloc) unrelated to this
 * frame-offset fix — so this example does not target x86_64. The host-
 * independent structural pin (tests/lir/test_lir_callconv.cpp
 * Aarch64HighStackParamUsesScaledImm12FrameLoad) guards the chokepoint
 * selection on EVERY CI leg; this example adds the arm64 RUN witness. arm64
 * runs under qemu-aarch64 on the linux leg and natively on the arm64 CI leg. */
int g(int p00, int p01, int p02, int p03, int p04, int p05, int p06, int p07,
      int p08, int p09, int p10, int p11, int p12, int p13, int p14, int p15,
      int p16, int p17, int p18, int p19, int p20, int p21, int p22, int p23,
      int p24, int p25, int p26, int p27, int p28, int p29) {
    return p28 + p29 + p08;
}

int main(void) {
    return g(0, 0, 0, 0, 0, 0, 0, 0,
             1, 0, 0, 0, 0, 0, 0, 0,
             0, 0, 0, 0, 0, 0, 0, 0,
             0, 0, 0, 0, 1, 40);
}
