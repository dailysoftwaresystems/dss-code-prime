// D-CSUBSET-CMP-COND-CORPUS (step 13.5 cycle 2, 2026-06-03 — test-
// analyzer 7-agent-review rec): bit-packed witness exercising all 6
// SIGNED TargetCondCode arms (Eq/Ne/Slt/Sle/Sgt/Sge) in one binary.
// Each `if` branch sets one bit of `r`; the final exit code uniquely
// encodes which arms branched correctly. A single wrong nibble in the
// per-target `condCodeEncoding[]` table flips exactly one bit and
// the bug names itself in the exit-code diff (vs the manifest's
// `exitCode: 22`).
//
// With a=5, b=7: a==b=0, a!=b=1, a<b=1, a<=b=1, a>b=0, a>=b=0.
// Expected exit code = 0 + 2 + 4 + 8 + 0 + 0 = 14.
//
// Unsigned arms (Ult/Ule/Ugt/Uge) await c-subset's `unsigned` type
// support (anchored D-CSUBSET-UNSIGNED-CMP-CORPUS).
//
// This corpus row becomes a future OPT1 (13.6) differential-
// verification gate: const-fold's `if (5 < 7)` rewrite hits all 6
// signed arms in ONE differential run. The 6-branch shape also
// stresses the BlockRel32 per-function patch list across multiple
// sequential branches in one function body, AND the jcc compound
// encoding (`0F 8x rel32; E9 rel32`) under repetition — catches
// off-by-one in the patch-offset accumulator that single-branch
// tests miss.

int main() {
    int a;
    int b;
    int r;
    a = 5;
    b = 7;
    r = 0;
    if (a == b) { r = r + 1;  }
    if (a != b) { r = r + 2;  }
    if (a <  b) { r = r + 4;  }
    if (a <= b) { r = r + 8;  }
    if (a >  b) { r = r + 16; }
    if (a >= b) { r = r + 32; }
    return r;
}
