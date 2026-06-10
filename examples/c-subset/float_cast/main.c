// FC2 (V2-4.X, 2026-06-10): THE float runtime witness — the first
// float VALUES ever observable at runtime in this compiler. Double
// arithmetic + explicit `(int)` casts in FUNCTION BODIES compute the
// process exit code (NEVER global initializers — HIR const-eval folds
// float globals and the corpus would witness nothing).
//
// Both literal DECODE paths are runtime-witnessed:
//   * C23 hex-float  0x1.8p3 = 1.5 x 2^3 = 12.0   (FC1 c2 decode)
//   * decimal forms  0.25, plus the fraction EDGE forms `.75`
//     (leading-dot) and `2.` (trailing-dot)
//
//   hexPart()  = (int)(0x1.8p3 + 0.25) = (int)12.25 = 12
//   edgePart() = (int)(.75 + 2.)       = (int) 2.75 =  2
//   exit       = combine(12, 2) = 12 + 2*2 = 16
//
// C truncation-toward-zero is discriminated by edgePart: a
// round-to-nearest lowering would yield 3 -> exit 18 != 16. A hex-float
// decode that misread the binary exponent (e.g. 1.8e3) lands far away.
//
// Fold-resistance, honestly stated: c-subset has NO float type KEYWORD
// (float values exist only via F64 literals), so float operands CANNOT
// flow through function parameters the way division/modulo route their
// ints — every float expression is literal-rooted by language
// construction. The resistance available is used: each float expression
// lives in its OWN function body (an intraprocedural pass in main never
// sees them) and the cast RESULTS cross call boundaries as runtime ints
// into `combine`, whose arithmetic no current or future ConstFold can
// fold without seeing through calls. MIR ConstFold is int-only today,
// so the FAdd + FPToSI (x86_64: ADDSD + CVTTSD2SI over RIP-relative
// .rodata F64 globals) execute at runtime on every arm.
//
// Arms: x86_64 PE (windows) + x86_64 ELF (linux) + arm64 ELF (linux,
// qemu-aarch64) + arm64 Mach-O (darwin). The PE arm was initially
// BLOCKED fail-loud: under ms_x64 (callee-saved xmm6-15 + the
// regalloc's callee-saved-first policy) the prologue needs an FPR
// saved-reg STORE, and the x86_64 registerClassOps declared no
// 'store' for class fpr -> L_RequiredLirOpcodeMissing (lir_callconv
// classOpHandle). Closed 2026-06-10 by declaring `movsd_store`
// (F2 0F 11 /r) + the fpr store row — this PE arm is the consumer
// that triggered it, and the FIRST float-value runtime witness on
// Windows. The arm64 arms landed with D-ARM64-FLOAT-SUBSTRATE
// (2026-06-10): d0..d31 fpr registers + AAPCS64/apple_arm64 FPR
// pools + FADD/FCVTZS/FMOV/LDUR(D)/STUR(D) encodings + the fpr
// registerClassOps row — declaration-only, zero engine change
// (x86_64: ADDSD + CVTTSD2SI; arm64: FADD + FCVTZS over the same
// promoted rodata F64 globals). No x86_64 darwin:
// macho64-x86_64-darwin-exec ships no processExit/cli profile.
int hexPart() { return (int)(0x1.8p3 + 0.25); }

int edgePart() { return (int)(.75 + 2.); }

int combine(int a, int b) { return a + 2 * b; }

int main() { return combine(hexPart(), edgePart()); }
