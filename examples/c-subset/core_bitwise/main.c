/* Cluster-F F2 (core_bitwise): every C bitwise / shift operator end-to-end.
 *
 * Each operator lives in its OWN function so its operands reach codegen as
 * RUNTIME values the optimizer cannot fold — the actual bitwise/shift machine
 * instruction executes (no const-fold short-circuit), even under the `release`
 * shipped pipeline (which does not inline these calls).
 *
 * PRIMARY witnesses (the encodings THIS cycle added):
 *   ^  ->  x86 XOR r/m,r (0x31 /r)               | arm64 EOR (already declared)
 *   ~  ->  x86 NOT r/m   (0xF7 /2)               | arm64 MVN = ORN Xd,XZR,Xm (0xAA2003E0)
 * plus the already-working & | << >> — and the SIGNED vs UNSIGNED right shift
 * distinction (`>>` on a signed operand = arithmetic / sign-extending `sar`/`asr`;
 * on an unsigned operand = logical / zero-filling `shr`/`lsr`).
 *
 * A missing/wrong encoding either fails to COMPILE (A_NoEncodingDeclared /
 * L_RequiredLirOpcodeMissing) or computes a wrong value -> a non-42 exit.
 * exit 42 == every operator correct on this target.
 */

int b_and(int a, int b) { return a & b; }
int b_or (int a, int b) { return a | b; }
int b_xor(int a, int b) { return a ^ b; }
int b_not(int a)        { return ~a; }
int b_shl(int a, int n) { return a << n; }
int b_sar(int a, int n) { return a >> n; }            /* signed: arithmetic right shift */
unsigned b_shr(unsigned a, int n) { return a >> n; }  /* unsigned: logical right shift   */

int main(void) {
    int a = 240, b = 51;                   /* 0xF0, 0x33 */
    int neg = -16;

    if (b_and(a, b) != 48)  return 1;      /* 0xF0 & 0x33 = 0x30 */
    if (b_or (a, b) != 243) return 2;      /* 0xF3 */
    if (b_xor(a, b) != 195) return 3;      /* 0xC3            <- XOR encoding witness */
    if ((b_not(a) & 255) != 15) return 4;  /* ~0xF0 low byte = 0x0F  <- NOT / MVN witness */
    if (b_shl(a, 1) != 480) return 5;      /* 240 << 1 */
    if (b_sar(neg, 2) != -4) return 6;     /* -16 >> 2 = -4 (arithmetic, sign-extends) */
    if (b_shr(240u, 2) != 60u) return 7;   /* 240 >> 2 = 60 (logical) */

    /* sar and shr are DISTINCT instructions: on a negative operand the
       arithmetic shift sign-extends (-4) while the logical shift zero-fills
       (a large positive). If they were the same opcode this would be equal. */
    if (b_sar(neg, 2) == (int)b_shr((unsigned)neg, 2)) return 8;

    return 42;
}
