// FC3.5 sweep-c3 (D-LK10-ENTRY-ARM64-WIDE-IMMEDIATE) runtime witness:
// constants ABOVE the arm64 MOVZ 16-bit window. Pre-sweep every one of
// these fail-louded at encode (A_ImmediateOperandOutOfRange); now the
// MIR->LIR const materialization splits them into the minimal
// MOVZ + MOVK ladder (x86 arms: unchanged single `mov r, imm32`).
//
//   big  = 196608 = 0x30000 — chunk0 (lo16) is ZERO, the value lives
//          ENTIRELY in the MOVK chunk: a dropped/mis-windowed MOVK
//          materializes 0 -> (0 >> 12) - 6 = -6, far from 42.
//   wide = 100000 = 0x186A0 — both chunks nonzero (MOVZ #0x86A0 +
//          MOVK #0x1, LSL #16).
//   sum  = 296608 = 0x486A0 — the comparison constant is ITSELF wide,
//          and the 64-bit compare sees every chunk: any chunk drift in
//          ANY of the three ladders exits 7.
//
//   exit = (196608 >> 12) - 6 = 48 - 6 = 42.
int main() {
    long big = 196608;
    long wide = 100000;
    long sum = big + wide;
    if (sum != 296608) { return 7; }
    return (int)(big >> 12) - 6;
}
