/* Cluster-F F3 (float_mul_sub): the float `*` and `-` operators end-to-end, on
 * double AND float.
 *
 * Each operator lives in its own function so its operands reach codegen as
 * RUNTIME values the optimizer cannot fold — the MUL/SUB instruction actually
 * executes, even under the `release` shipped pipeline (which does not inline).
 *
 * Witnesses the encodings THIS cycle added:
 *   *  ->  x86 MULSD/MULSS (F2/F3 0F 59)  | arm64 FMUL (0x1E600800 / 0x1E200800)
 *   -  ->  x86 SUBSD/SUBSS (F2/F3 0F 5C)  | arm64 FSUB (0x1E603800 / 0x1E203800)
 *
 * SUBTRACTION IS NON-COMMUTATIVE: pinned WITHOUT a negative literal (which would
 * need the out-of-scope `fneg`) via (a-b) - (b-a) == 2*(a-b): if the operand wire
 * were reversed, a-b and b-a would be EQUAL and the difference would be 0, not 84.
 * A missing/wrong encoding fails to COMPILE or computes a wrong value -> non-42.
 * exit 42 == every operator + operand order correct on this target.
 */

double dmul(double a, double b) { return a * b; }
double dsub(double a, double b) { return a - b; }
float  fmul(float a, float b)   { return a * b; }
float  fsub(float a, float b)   { return a - b; }

int main(void) {
    /* double */
    if (dmul(6.0, 7.0) != 42.0)  return 1;   /* 42.0 (exactly representable) */
    if (dsub(50.0, 8.0) != 42.0) return 2;   /* a - b = 42 */
    /* non-commutativity, no negative literal: (a-b) - (b-a) = 42 - (-42) = 84.
       A swapped operand wire makes a-b == b-a -> the difference is 0 != 84. */
    if (dsub(50.0, 8.0) - dsub(8.0, 50.0) != 84.0) return 3;

    /* float */
    if (fmul(3.0f, 14.0f) != 42.0f) return 4;
    if (fsub(50.0f, 8.0f) != 42.0f) return 5;
    if (fsub(50.0f, 8.0f) - fsub(8.0f, 50.0f) != 84.0f) return 6;

    /* a compound that equals 42 only if every op is right */
    double r = dmul(2.0, 3.0) + dsub(45.0, 9.0);  /* 6 + 36 = 42 */
    if (r != 42.0) return 7;

    return (int)r;                             /* 42 */
}
