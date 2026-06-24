// D-WIN64-LARGE-FRAME-STACK-PROBE witness: a Win64 (x86_64 + PE, ms_x64) function
// whose frame EXCEEDS one Windows guard page (4096B). `int big[9000]` = a 36000-byte
// local array → the prologue must descend RSP ~9 pages below entry. A bare
// `sub rsp, 36000+` skips the single PAGE_GUARD page Windows reserves the stack
// behind, so the FIRST write into the deep frame access-violates
// (STATUS_GUARD_PAGE_VIOLATION → exit 139/0xC0000005). The fix emits an INLINE
// stack-probe loop (the `stack_probe` LIR op, lowered by the x86_64 encoder) that
// TOUCHES every page on the way down, committing each guard page so the deep
// writes are safe.
//
// `seed` is a mutable global so the store→load round-trip survives
// ConstFold/Mem2Reg/DCE (the array cannot be optimized away). This is a NATIVE
// Windows run: WITHOUT the probe it SIGSEGVs (exit 139); WITH it, exit 42.
// x86_64:elf (Linux) does NOT need the probe — Linux auto-grows the stack via
// demand paging — so this example targets the Windows pe64 leg specifically (the
// arm64/elf large-frame path is covered by large_frame_arm64).

int seed = 42;

int main(void) {
    int big[9000];
    big[0]    = seed;        // low-page write (near entry RSP)
    big[8999] = seed;        // deep write (~9 pages down — the guard-skip fault site)
    return big[8999];
}
