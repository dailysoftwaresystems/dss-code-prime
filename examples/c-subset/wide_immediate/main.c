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
//
// audit-residue sweep c2 (D-AUDIT-WITNESS-STRENGTHENING): every value
// above stops at movk_lsl16 — chunks 2/3 (movk_lsl32 / movk_lsl48)
// had NO runtime traffic. negCheck() adds it via the width-64
// NEGATIVE materialization: the sign-extended pattern = chunk0 +
// 0xFFFF-FILLED high chunks -> the FULL 4-op ladder
//   MOVZ #0x2979 + MOVK #0xFFED,LSL16 + MOVK #0xFFFF,LSL32
//                + MOVK #0xFFFF,LSL48        (-1234567)
// (the c3 design's deliberate non-MOVN form). REACHABILITY, honestly:
// NO c-subset source shape mints a negative inline I64 const in the
// BASELINE pipeline — a `-`/`0ll - N` form lowers as a runtime NEG/SUB
// of the POSITIVE const (empirically probed) — so the manifest's
// constfold-only arm carries this witness: ConstFold folds
// `0ll - 1234567ll` into Const(-1234567) (the FoldsSubOfTwoConstants
// pin) and the MIR->LIR ladder shape is pinned by
// Arm64NegativeConstEmitsFullFourOpLadder; this arm EXECUTES it.
// OBSERVATION is the 64-bit compare, never exit arithmetic: a
// dropped/mis-windowed high MOVK shifts the value by k*2^32 —
// invisible mod 2^32 AND mod 256 (the POSIX exit byte) — but
// -1234567 + 1234609 == 42 EXACTLY only when every chunk lands; any
// drift -> != 42 -> exit 7 (42 vs 7, delta 35, WEXITSTATUS-safe).
// The baseline arm computes the same value at runtime (exit-equal,
// differential-verified); x86 arms: -1234567 fits the sign-extended
// `mov r64, imm32` single-op path, unchanged. The fix operand 1234609
// = 0x12D6B1 arrives as a CALL ARG (its own independent 2-op ladder;
// keeps the add live under ConstFold).
long long negCheck(long long fix) {         // called as negCheck(1234609)
    long long neg = 0ll - 1234567ll;        // constfold arm: Const(-1234567)
                                            //   = 0xFFFFFFFFFFED2979
    return neg + fix;                       // 42 iff every chunk landed
}

int main() {
    long big = 196608;
    long wide = 100000;
    long sum = big + wide;
    if (sum != 296608) { return 7; }
    if (negCheck(1234609) != 42) { return 7; }
    return (int)(big >> 12) - 6;
}
