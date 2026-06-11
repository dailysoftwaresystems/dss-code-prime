// FC3.5 sweep-c2 (D-CSUBSET-F32-CODEGEN closed): the FIRST F32
// runtime witness — `float` values computed at single precision
// end-to-end. Exercises, on every arm:
//   * the f-suffix literal typing (`40.25f` → F32 — the
//     floatLiteralTyping flip of the FC3-c1 interim F64 pin),
//   * F32 literal MATERIALIZATION (4-byte .rodata items + the
//     width-32 movss / LDUR-S loads — an 8-byte read would cross the
//     item),
//   * ADDSS / arm64 FADD-S (the F3-prefix / ftype=00 width-32
//     variants),
//   * the F64↔F32 conversions BOTH ways: `f + 0.5` mixes F32+F64 →
//     UAC widens via FPExt (CVTSS2SD / FCVT D←S) and ADDSD computes
//     at double; `(float)d` narrows via FPTrunc (CVTSD2SS / FCVT
//     S←D),
//   * CVTTSS2SI / FCVTZS-from-S (the F32-SOURCE fp_to_si variant —
//     the source-width axis, not the I32 result's).
//
// Every fractional part (.25, .5, 1.0) is exactly representable in
// binary32 AND binary64, so the chain is exact:
//   add(40.25f, 1.0f) = 41.25f          (ADDSS)
//   widen(41.25f)     = 41.75 (double)  (CVTSS2SD + ADDSD)
//   narrow(41.75)     = 41.75f          (CVTSD2SS)
//   (int)(41.75f + 0.25f) = (int)42.0f  = 42  (ADDSS + CVTTSS2SI)
//
// HONEST REACH of the exact chain (audit-residue sweep c2,
// D-AUDIT-WITNESS-STRENGTHENING): BECAUSE every value is exact in
// both binary32 and binary64, computing the whole chain at the wrong
// precision (an all-F64 misinterpretation — a broken width axis)
// would still exit 42 — the chain witnesses the PLUMBING (loads,
// conversions, callconv), not the precision. The PRECISION witness is
// the round_away() arm below: 2^24 + 1 is the first integer binary32
// cannot represent, so under a true ADDSS/FADD-S the +1.0f rounds
// away (round-to-nearest-even -> 16777216.0f) and the result EQUALS
// big, while a double-precision add holds 16777217.0 exactly and the
// equality flips -> exit 7 (42 vs 7, delta 35, WEXITSTATUS-safe).
// The 16777216.0f literal rides the F32 rodata materialization path
// (float literals are never inline int32 consts), and the F32 `==`
// rides the width-32 fcmp variants (UCOMISS / FCMP Sn,Sm).
// Selecting the WRONG conversion direction (the fpcvt source-width
// axis) still type-breaks the chain at the callconv boundary, and a
// movss/movsd width slip on the 4-byte rodata items reads the
// neighboring item's bytes into the high half (the boundary-fault
// hazard is the reason the width axis exists — pinned at the byte
// tier).
//
// Fold-resistance: float operands arrive as FUNCTION PARAMETERS;
// MIR ConstFold is int-only today, and the cast result crosses a
// call boundary only as the final exit code.
float add(float a, float b) { return a + b; }

double widen(float f) { return f + 0.5; }

float narrow(double d) { return (float)d; }

// The binary32 ROUNDING discriminator: big + 1.0f at single precision
// rounds the +1 away (ties-to-even at the 2^24 representability
// edge); at double precision it does not. The operand arrives as a
// FUNCTION PARAMETER (fold-resistant, like every arm above).
float round_away(float big) { return big + 1.0f; }

int main() {
    float s = add(40.25f, 1.0f);
    double w = widen(s);
    float n = narrow(w);
    float big = 16777216.0f;                 // 2^24, exact in binary32
    if (round_away(big) == big) {            // true ONLY at binary32
        return (int)(n + 0.25f);             // the exact-chain 42
    }
    return 7;                                // double-precision add leaked in
}
