// Regression corpus for D-CODEGEN-FTS3-UNICODE61-TOKENIZER-END-OFFSET-OFF-BY-ONE.
//
// THE SHAPE: a `do { CAPTURE; ADVANCE; } while (COND);` loop, where CAPTURE
// copies the induction variable at the TOP of the body and ADVANCE moves it at
// the bottom, so the captured value must LAG the cursor by exactly one step —
// and the capture is read AFTER the loop. This is sqlite's fts3 unicode61
// tokenizer (ext/fts3/fts3_unicode.c unicodeNext: `zEnd = z;` at the top of the
// do-body, `READ_UTF8(z, ...)` at the bottom, `*piEnd = zEnd - aInput` after).
//
// ✔MEASURED 2026-08-07: DSS release returned the value of the CURSOR instead of
// the value of the CAPTURE — the first token of "abc\x01xyz" came back with end
// offset 4 instead of 3, so fts3 and fts5 disagreed and upstream's fts5tok2.test
// (which asserts the two tokenizers are byte-for-byte identical) failed at its
// very first input. gcc -O0 and -O2 are both correct; DSS --config=debug is
// correct; only DSS --config=release was wrong.
//
// ★ ROOT CAUSE — the LOST-COPY PROBLEM ON A CRITICAL EDGE, in SSA DESTRUCTION,
//   not in any optimizer pass: `mir_to_lir.cpp` emitted a phi's edge copies in
//   the PREDECESSOR, before the terminator, for EVERY successor. A block with
//   two successors therefore wrote the phi register even on the arm it did not
//   take, and the loop-exit block read the clobbered value.
// ⚠ THE BISECT SAID "Mem2Reg" AND THE BISECT WAS RIGHT ABOUT THE OBSERVATION AND
//   WRONG ABOUT THE CULPRIT — a lesson worth more than the fix. Dropping Mem2Reg
//   from the pipeline does make this correct, because without SSA promotion
//   there is no phi, and with no phi there is no phi-copy to misplace. "Removing
//   pass X fixes it" means X CREATES THE CONDITION, which is not the same as X
//   being wrong; here Mem2Reg's output was correct at every point. Only the
//   LIR dump settled it, by showing `p14 = mov p13` sitting above the `jcc` in a
//   block whose successor list had two entries.
//
// ⚠ WHY THE CORPUS DID NOT ALREADY CATCH THIS: of 565 examples, exactly ONE used
// `do`/`while` — nonmain_dowhile_inf_return, which is `do { return 7; } while(1)`.
// That has no loop-carried variable at all and a CONSTANT-TRUE condition, and a
// constant-true do-while is a shape that was measured CORRECT. The defect needs
// a REAL bottom test plus a lagging capture, and nothing in the corpus had both.
//
// ★ EVERY INPUT IS RUNTIME-DERIVED (from argc) so no pass can constant-fold the
//   answers away and leave this witnessing nothing.
// ★ EACH SHAPE CONTRIBUTES A DISTINCT BIT so a failure NAMES which one broke,
//   rather than reporting a single anonymous wrong exit code.

// (1) the reduced shape: `e` must be `i` as it was at the top of the last pass.
static int lag_int(int n) {
    int i = 0, e = -1;
    do {
        e = i;
        i++;
    } while (i < n);
    return e;                       /* n-1, NOT n */
}

// (2) TWO captures in one body — both must lag, and independently.
static int lag_two(int n, int *pg) {
    int i = 0, e = -1, g = -1;
    do {
        e = i;
        g = i + 10;
        i++;
    } while (i < n);
    *pg = g;                        /* n-1+10 */
    return e;                       /* n-1 */
}

// (3) the tokenizer shape itself: pointers, a mid-body break, and the capture
//     read after the loop. `sep` is the byte that ends the first token.
static int first_token_end(const unsigned char *in, int n, int sep) {
    const unsigned char *z = in;
    const unsigned char *zTerm = in + n;
    const unsigned char *zEnd = 0;
    int c = 0;
    do {
        zEnd = z;                   /* lags z by one byte */
        if (z >= zTerm) break;
        c = *z++;
    } while (c != sep);
    return (int)(zEnd - in);
}

int main(int argc, char **argv) {
    (void)argv;
    int fail = 0;

    /* argc is 1 under the runner, so n is 5 and the answer is 4. */
    if (lag_int(argc + 4) != 4) fail |= 1;

    int g = 0;
    if (lag_two(argc + 4, &g) != 4) fail |= 2;
    if (g != 14)                    fail |= 4;

    /* "abc" + sep + "xyz" — the separator sits at index 3, so the first token
       ends at 3. Returning 4 is the measured defect: the separator included. */
    unsigned char buf[8];
    int n = 0;
    buf[n++] = 'a'; buf[n++] = 'b'; buf[n++] = 'c';
    buf[n++] = (unsigned char)argc;         /* the separator, value 1 */
    buf[n++] = 'x'; buf[n++] = 'y'; buf[n++] = 'z';
    if (first_token_end(buf, n, argc) != 3) fail |= 8;

    return fail == 0 ? 42 : fail;
}
