/* D-OPT11-LAZY-IMPORT-EDGE — a call chain FOUR translation units deep.
 *
 * The point of the depth: an EAGER cross-CU import plan is computed from the
 * summaries before anything is optimized and is bounded by a prefetch depth, so
 * at depth 1 it can offer `main` exactly one body — `level1`. `level2` and
 * `level3` live two and three call-graph levels out and no eager plan at that
 * depth ever names them. A LAZY edge keeps asking as the module keeps naming
 * callees it does not have, so it reaches all three.
 *
 * ★ THE EXIT CODE WITNESSES THE VALUES, NOT MERELY THAT THE LINK SUCCEEDED.
 * 3 -> +4 -> *3 -> *2 = 42, and every arm is a different operation, so a body
 * spliced from the wrong TU, in the wrong order, or with a dropped argument
 * lands on a different number rather than on a plausible one.
 */
int level1(int x);

int main(void) { return level1(3); }
