// D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB witness: a function with a frame LARGER than
// the AArch64 2-word shifted-imm12 reach (0xFFFFFF = 16 MiB). `int big[5000000]` =
// 20,000,000 bytes → both the prologue/epilogue SP adjust AND the high-element address
// `&big[4999999]` (byte offset 19,999,996) exceed the 16 MiB shifted-imm12 ceiling.
//
// The fix is the 3-word MOVZ/MOVK + EXTENDED-register macro: a value V in
// (0xFFFFFF, 0x7FFFFFFF] encodes as `MOVZ Xs,#(V & 0xFFFF)` + `MOVK Xs,#((V>>16) & 0xFFFF),
// LSL #16` + an EXTENDED-register op. The prologue's `sub sp,sp,#frame` becomes
// `MOVZ x16,#lo ; MOVK x16,#hi,LSL#16 ; sub sp,sp,x16` (x16 = AAPCS64 IP0 scratch, free
// at frame setup); the epilogue mirrors it with `add sp,sp,x16`; the `lea` for
// &big[4999999] becomes `MOVZ x_d,#lo ; MOVK x_d,#hi,LSL#16 ; add x_d,sp,x_d` (SCRATCH-FREE
// — the displacement materializes into the lea's OWN dest reg). A frame > 2 GiB stays
// fail-loud (D-ASM-AARCH64-FRAME-OFFSET-BEYOND-2GIB — the int32 frame size goes negative).
//
// `seed` is a mutable global so the store→load round-trip survives ConstFold/Mem2Reg/DCE
// (the array cannot be optimized away). big[4999999] is read BACK to exercise the
// high-element `lea` tier (not just the sp-adjust). Runs on x86_64 (disp32 — already fine)
// AND qemu-aarch64 (the new 3-word materialization). Exit 42.
//
// The 20MB stack frame SIGSEGVs under the default 8MB ulimit; the examples runner sets
// QEMU_STACK_SIZE=256M (+ RLIMIT_STACK on POSIX) in the parent so the child inherits a
// generous stack — see tests/examples/examples_runner.cpp.

int seed = 42;

int main(void) {
    int big[5000000];
    big[0] = seed;
    big[4999999] = seed;
    return big[4999999];
}
