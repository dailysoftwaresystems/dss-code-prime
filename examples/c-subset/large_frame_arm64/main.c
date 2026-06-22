// D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12 witness: a function with a frame LARGER than
// the AArch64 single-word `add`/`sub sp,#imm12` reach (4095). `int big[9000]` = a 36000-byte
// local array → both the prologue/epilogue SP adjust AND the high-element address
// `&big[8999]` (byte offset 35996) exceed the single-word imm12 field.
//
// The fix is SCRATCH-FREE shifted-imm12: a value V in (4095, 0xFFFFFF] encodes as a 2-word
// ADD/SUB macro — `op Xd,Xn,#(V & 0xFFF)` (sh=0) then `op Xd,Xd,#(V>>12),LSL #12` (sh=1, the
// LSL #12 form). The prologue's `sub sp,sp,#frame` becomes two SUBs (the low 12 bits, then
// the high 12 bits shifted); the epilogue mirrors it with two ADDs; the `lea` for &big[8999]
// becomes `ADD Xd,sp,#low` + `ADD Xd,Xd,#high,LSL #12` (word1 reads its OWN dest — no scratch
// register). This is NOT a MOVZ/MOVK + register-form `sub sp,sp,Xn` materialization (a
// different mechanism this fix deliberately avoids); the shifted-imm12 reaches 16 MiB
// scratch-free, and a frame > 16 MiB stays fail-loud (D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB).
//
// `seed` is a mutable global so the store→load round-trip survives ConstFold/Mem2Reg/DCE
// (the array cannot be optimized away). Runs on x86_64 (disp32 — already fine) AND
// qemu-aarch64 (the large-frame shifted-imm12 materialization). Exit 42.

int seed = 42;

int main(void) {
    int big[9000];
    big[8999] = seed;
    return big[8999];
}
