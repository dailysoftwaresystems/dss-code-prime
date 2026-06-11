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
// A width mix-up is value-visible: computing the chain at the wrong
// precision cannot round these exact values away, but selecting the
// WRONG conversion direction (the fpcvt source-width axis) swaps
// widen/narrow and the chain type-breaks at the callconv boundary —
// while a movss/movsd width slip on the 4-byte rodata items reads
// the neighboring item's bytes into the high half (the value stays
// correct only because consumers read the low 4 bytes; the
// boundary-fault hazard is the reason the width axis exists — pinned
// at the byte tier, witnessed here as plumbing).
//
// Fold-resistance: float operands arrive as FUNCTION PARAMETERS;
// MIR ConstFold is int-only today, and the cast result crosses a
// call boundary only as the final exit code.
float add(float a, float b) { return a + b; }

double widen(float f) { return f + 0.5; }

float narrow(double d) { return (float)d; }

int main() {
    float s = add(40.25f, 1.0f);
    double w = widen(s);
    float n = narrow(w);
    return (int)(n + 0.25f);
}
