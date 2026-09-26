/* D-CSUBSET-LONG-BRANCH — the ±1 MiB AArch64 `Imm19` escape, end to end, with
 * its control in the SAME binary.
 *
 * ★★★ WHY THE SOURCE IS A DOZEN LINES AND NOT A GENERATED MEGABYTE. Reaching
 * an out-of-range block-relative branch needs >1 MiB of EMITTED CODE between a
 * conditional branch and its target. It does not need a large SOURCE: this file
 * is 462 bytes of code, and nested object-like repetition expands in the
 * preprocessor, so it stays readable, diffable and reproducible by construction
 * — there is no generator to ship and nothing to regenerate by hand.
 *
 * ★★★ WHY A `do { } while` AND NOT AN `if`, AND IT IS THE WHOLE INSTRUMENT.
 * ✔MEASURED at this tree: an `if (gate) { <1.3 MiB body> }` emits
 * `b.ne <fallthrough>; b <far>` — a TWO-WORD form whose far target already
 * rides the ±128 MiB `Imm26` word. Its `Imm19` therefore never goes out of
 * range and NO escape is elected, in a program that looks for all the world
 * like it must have taken the path. A `do { } while` latch is the shape that
 * actually reaches it: the loop head is the TAKEN target and the loop exit is
 * the fallthrough, so the latch emits the ONE-WORD `b.cond <head>` form — and
 * that word's field is the `Imm19`.
 *
 * THE PAIR, and both halves are in this one program on purpose: they compile
 * under the same driver, the same optimizer configuration and the same
 * assembler pass, so the only thing that differs between them is the distance.
 *
 *   dss_lbs_near_latch  1024 copies  ~0.64 MiB body — the `Imm19` REACHES,
 *                                    and the latch stays one word.
 *   dss_lbs_far_latch   2048 copies  ~1.28 MiB body — the `Imm19` CANNOT reach,
 *                                    so the resolver promotes the instruction
 *                                    and re-encodes it as the two-word escape.
 *
 * ✔MEASURED (aarch64-linux-gnu-objdump, this tree, BOTH pipelines) — the exact
 * shapes the manifest's note quotes: the near latch ends `b.ne <head>` followed
 * by its own `mov w0, #1`, and the far latch ends `b.eq +8` followed by
 * `b <head>` — the condition INVERTED and the far target moved onto the wide
 * word, which is `electEscapeWord` quoting the arm64 jcc row's own trailing
 * `B` rather than synthesizing an encoding.
 *
 * ★★ THE STATEMENT IS A VOLATILE AGGREGATE COPY, AND THAT CHOICE IS LOAD-
 * BEARING TWICE. It is dense — ~655 emitted bytes per copy, so 1.28 MiB costs
 * 2048 statements rather than tens of thousands, which is the difference
 * between an affordable corpus entry and an unaffordable one. And it is
 * IRREDUCIBLE: `volatile` forbids the optimizer from eliding, merging or
 * hoisting any of it, so the release arm crosses the same boundary the
 * baseline arm does instead of shrinking back under it. ✔MEASURED: the release
 * images differ from the baseline ones and BOTH still show the escape.
 *
 * RUNTIME. `dss_lbs_gate` is zero, so each loop runs its body exactly once and
 * falls out — the latch is EXECUTED, not merely encoded. A displacement wrong
 * by even one word lands control inside the copy sequence or past the return,
 * so exit 42 is a witness that the escaped branch resolves correctly and not
 * just that the assembler was willing to emit something.
 */

volatile int dss_lbs_gate;

struct wide { long a[32]; };
volatile struct wide dss_lbs_p;
volatile struct wide dss_lbs_q;

#define S        dss_lbs_p = dss_lbs_q;
#define R4(x)    x x x x
#define R16(x)   R4(R4(x))
#define R256(x)  R16(R16(x))
#define R1024(x) R4(R256(x))

/* CONTROL — under the boundary. Its latch must NOT be escaped. */
int dss_lbs_near_latch(void) {
    do {
        R1024(S)
    } while (dss_lbs_gate);
    return 1;
}

/* SUBJECT — over the boundary. Its latch must BE escaped. */
int dss_lbs_far_latch(void) {
    do {
        R1024(S)
        R1024(S)
    } while (dss_lbs_gate);
    return 41;
}

int main(void) {
    return dss_lbs_near_latch() + dss_lbs_far_latch();
}
