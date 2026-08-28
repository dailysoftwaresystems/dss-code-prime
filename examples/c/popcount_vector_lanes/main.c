// D-FULLC-STDBIT-ARM64-CNT-POPCOUNT: the runtime witness for a population count
// realized as a DECLARED INSTRUCTION SEQUENCE rather than one instruction.
//
// WHAT DIFFERS PER TARGET, and why the same exit code on both is the proof:
//   x86-64 : POPCNT — one instruction, the degenerate one-step case.
//   arm64  : no scalar-GPR population count EXISTS, so the target declares a
//            four-step sequence — a GPR->SIMD move, a PER-BYTE lane count, a
//            lane REDUCTION, and a SIMD->GPR move.
// Two completely different machine realizations must agree on every value here.
// That agreement is the whole assertion: this file never mentions an
// instruction, an architecture or a lane, and it is the `.target.json` that
// decides which of the two runs.
//
// ★ THE INPUTS ARE CHOSEN TO KILL THE SPECIFIC WAYS A LANE-BASED POPCOUNT GOES
// WRONG, not merely to be "some numbers". A per-byte counter followed by a
// horizontal reduction has failure modes a scalar POPCNT does not have, and a
// test whose inputs cannot tell them apart is green for the wrong reason:
//
//   A  0xFF00000000000000 -> 8   ★ THE REDUCTION PROBE. Every set bit is in the
//                                HIGHEST byte, so lane 0 counts ZERO. Drop the
//                                horizontal reduction and this reads 0, not 8.
//   B  0x0102040810204080 -> 8   ★ ALL EIGHT LANES. One bit per byte, so the
//                                answer is only right if every lane is summed;
//                                lane 0 alone gives 1.
//   C  0xFFFFFFFFFFFFFFFF -> 64  ★ THE ACCUMULATOR WIDTH. Eight lanes of eight
//                                is the maximum a byte-wide reduction target
//                                must hold. 64 fits an unsigned byte with room
//                                to spare; a reduction that saturated or
//                                truncated would show here and nowhere else.
//   D  0xFFFFFFFF00000000 -> 32  ★ THE WIDTH PROBE. Every set bit is in the HIGH
//                                half, so running the 64-bit count at 32 bits
//                                answers 0.
//   E  0xFFFFFFFF (32-bit) -> 32 ★ THE UPPER-LANE PROBE. A 32-bit value occupies
//                                four lanes; the other four must read ZERO. If
//                                they held anything from a previous use of that
//                                register this exceeds 32.
//   F  0x80000000 (32-bit) -> 1  the top bit alone, at the narrow width.
//   G  0x00FF00FF (32-bit) -> 16 byte-spread at the narrow width.
//   H  0x00000001 (32-bit) -> 1  the low bit alone.
//
// exit = 8 + 8 + 64 + 32 + 32 + 1 + 16 + 1 = 162, HAND-DERIVED from the values
// above and not from what DSS printed.
//
// Fold-resistant: every count runs inside a wrapper fn taking the value as an
// ARG, so the baseline pipeline's ConstFold never sees a literal; and Popcount
// is deliberately NOT in ConstFold's fold set, so the `release` arm inlines the
// wrappers and the ops still stay LIVE. The sequence is therefore exercised
// through the full optimizer as well as the baseline.

typedef unsigned int       u32;
typedef unsigned long long u64;

static int pc32(u32 x) { return __builtin_popcount(x); }
static int pc64(u64 x) { return __builtin_popcountll(x); }

int main(void) {
    u64 a = 0xFF00000000000000ull;   // 8  — reduction probe (lane 0 is empty)
    u64 b = 0x0102040810204080ull;   // 8  — one bit in each of the eight lanes
    u64 c = 0xFFFFFFFFFFFFFFFFull;   // 64 — the reduction's maximum
    u64 d = 0xFFFFFFFF00000000ull;   // 32 — high half only; a 32-bit count = 0
    u32 e = 0xFFFFFFFFu;             // 32 — upper lanes must read zero
    u32 f = 0x80000000u;             // 1  — top bit, narrow width
    u32 g = 0x00FF00FFu;             // 16 — byte-spread, narrow width
    u32 h = 0x00000001u;             // 1  — low bit

    int total = 0;
    total += pc64(a);
    total += pc64(b);
    total += pc64(c);
    total += pc64(d);
    total += pc32(e);
    total += pc32(f);
    total += pc32(g);
    total += pc32(h);
    return total;   // 162
}
