/* D-ML7-2.3 (parallel-copy resolution) x D-ML7-2.5 (outgoing-argument
 * pre-coloring), P40 lane L.
 *
 * The pre-existing `arg_move_cycle` example witnesses ONE shape: an 8-element
 * FP rotation that fills every FP argument register exactly. It leaves four
 * shapes of the same mechanism unwitnessed at runtime, and this example adds
 * them on the GPR side, where the outgoing-argument pre-coloring makes the
 * cycles ROUTINE rather than accidental:
 *
 *   S1  a 2-cycle -- the textbook swap `t2(b, a)`.  The move set is
 *       {argreg0 <- argreg1, argreg1 <- argreg0}; no ordering of two moves is
 *       correct, so the resolver must save one source into a caller-saved
 *       scratch of the class and complete the swap through it.
 *   S2  a 3-cycle -- a rotation `t3(b, c, a)`, proving the break is not
 *       special-cased to length 2.
 *   S3  a cycle PLUS AN ACYCLIC TAIL -- `t3(b, a, c*3)`.  The first two
 *       arguments cycle; the third is a computed value that is nobody's
 *       source.  The resolver must emit the acyclic move in dependency order
 *       AND break the cycle, in one move set.  If the tail were emitted inside
 *       the cycle break (or the break chose the tail's destination as its
 *       scratch) the third argument would be destroyed.
 *   S4  MORE ARGUMENTS THAN ARGUMENT REGISTERS -- nine arguments with a
 *       leading swap.  Every shipped ABI passes fewer than nine integers in
 *       registers (SysV 6, Win64 4, AAPCS64 8), so the register cycle and the
 *       stack-passed overflow are exercised TOGETHER.  The stack stores must
 *       read their sources BEFORE the register moves clobber anything.
 *
 * Every callee positionally weights its parameters by a distinct power of two,
 * so ANY permutation error changes the sum -- a mis-ordered move cannot cancel
 * out.  Each shape reports its own exit code, so a failure names the shape.
 *
 * Inputs come from a VOLATILE global rather than literals so the values are
 * non-constant and the `release` arm cannot fold the calls away; the arguments
 * therefore travel through real registers in both arms.
 *
 * RED-ON-DISABLE: remove the cycle-break in `emitParallelRegMoves` (emit the
 * moves in order) and S1/S2/S3/S4 each read a clobbered source -- a SILENT
 * wrong answer, which is why each shape carries its own exit code rather than
 * a shared one.  => 42. */
volatile int one = 1;

int t2(int a, int b) { return a * 1 + b * 2; }

int t3(int a, int b, int c) { return a * 1 + b * 2 + c * 4; }

int t9(int a, int b, int c, int d, int e,
       int f, int g, int h, int i) {
    return a * 1 + b * 2 + c * 4 + d * 8 + e * 16
         + f * 32 + g * 64 + h * 128 + i * 256;
}

/* S1: a 2-cycle. */
int s1(int a, int b) { return t2(b, a); }

/* S2: a 3-cycle (rotate left). */
int s2(int a, int b, int c) { return t3(b, c, a); }

/* S3: a 2-cycle plus an acyclic tail (`c*3` is nobody's source). */
int s3(int a, int b, int c) { return t3(b, a, c * 3); }

/* S4: a leading 2-cycle with more arguments than any shipped ABI passes in
 * registers, so the register permutation and the stack overflow compose. */
int s4(int a, int b, int c, int d, int e,
       int f, int g, int h, int i) {
    return t9(b, a, c, d, e, f, g, h, i);
}

int main(void) {
    int u = one;  /* 1, but non-constant */

    /* t2(2,1) = 2*1 + 1*2 = 4 */
    if (s1(1 * u, 2 * u) != 4) return 1;

    /* t3(2,3,1) = 2*1 + 3*2 + 1*4 = 12 */
    if (s2(1 * u, 2 * u, 3 * u) != 12) return 2;

    /* t3(2,1,9) = 2*1 + 1*2 + 9*4 = 40 */
    if (s3(1 * u, 2 * u, 3 * u) != 40) return 3;

    /* t9(2,1,3,4,5,6,7,8,9)
     *  = 2*1 + 1*2 + 3*4 + 4*8 + 5*16 + 6*32 + 7*64 + 8*128 + 9*256
     *  = 2 + 2 + 12 + 32 + 80 + 192 + 448 + 1024 + 2304 = 4096 */
    if (s4(1 * u, 2 * u, 3 * u, 4 * u, 5 * u,
           6 * u, 7 * u, 8 * u, 9 * u) != 4096) return 4;

    return 42;
}
