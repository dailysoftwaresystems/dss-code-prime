/* D-OPT-JCC-FALLTHROUGH — the RUNTIME half of the fallthrough-elision pin.
 *
 * `lir_peephole`'s rule R2 stops a conditional branch from materializing its
 * FALLTHROUGH edge when that successor is already the next-laid-out block, so
 * the branch encodes one target instead of two (x86_64: 6 bytes instead of 11;
 * arm64: one word instead of two). The CFG keeps both edges — only the byte
 * that re-states the fallthrough goes away.
 *
 * ★★★ WHY A RUNNABLE EXAMPLE AND NOT ONLY A UNIT TEST. The failure mode this
 * optimization can produce is INVISIBLE to every structural check: elide the
 * jump and let the block order change, or elide the WRONG edge, and control
 * falls into a block it never branched to — with every individual byte in the
 * image correct. There is nothing for a disassembler, a linker or a verifier
 * of shapes to point at. The only witness is a program that computes a
 * different number, so each decision below owns ONE BIT of the exit code and
 * every wrong-way branch sets bit 6 (64), which no correct run ever sets.
 *
 * The shapes are chosen to give the layout BOTH answers: an `if`/`else` (the
 * false edge is the else block), an `if` with no else (the false edge is the
 * join), a `while` (the false edge leaves the loop), a short-circuiting `&&`
 * (which mints its own extra blocks), and a `for` with an inner `if`. Between
 * them the fallthrough successor is sometimes the next block laid out and
 * sometimes not, so BOTH arms of R2 — elide and refuse — run here.
 *
 * `volatile` on every input keeps ConstFold from evaluating the branches away,
 * so the `release` arm exercises the same conditional branches as the
 * baseline one rather than a straight line.
 *
 * exit = 1|2|4|8|16|32 = 63.
 */

volatile int v0 = 0;
volatile int v1 = 1;

int main(void) {
    int r = 0;

    /* A. if/else, condition TRUE — the taken edge runs, the else block does not. */
    if (v1) { r |= 1; } else { r |= 64; }

    /* B. if/else, condition FALSE — the FALLTHROUGH edge runs. */
    if (v0) { r |= 64; } else { r |= 2; }

    /* C. if with no else: the false edge IS the join block. */
    if (v1) { r |= 4; }
    if (v0) { r |= 64; }

    /* D. a loop — the exit edge and the body edge are the two successors, and
     *    the back edge makes the body's terminator jump BACKWARD. */
    int i = 0;
    while (i < 3) { i = i + 1; }
    if (i == 3) { r |= 8; } else { r |= 64; }

    /* E. short-circuit `&&` — two conditional branches sharing a join. */
    if (v1 && !v0) { r |= 16; } else { r |= 64; }

    /* F. a counted loop with an inner branch: 1 + 3 = 4. */
    int acc = 0;
    for (int k = 0; k < 4; k = k + 1) {
        if (k & 1) { acc = acc + k; }
    }
    if (acc == 4) { r |= 32; } else { r |= 64; }

    return r;
}
