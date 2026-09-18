/* D-CSUBSET-LONG-BRANCH — the AArch64 BRANCH ISLAND edge, taken deliberately.
 *
 * ★★★ WHY THIS IS NOT A CORPUS EXAMPLE. The ±1 MiB `Imm19` escape is affordable
 * and IS an ordinary corpus example (`examples/c/long_branch_imm19_escape`).
 * The ISLAND is what the WIDEST field has instead of an escape: `Imm26` is the
 * widest block-relative field AArch64 declares, so a `b` that cannot reach its
 * target has no wider word to be promoted into and the resolver must place a
 * LANDING PAD part of the way there instead. That edge is ±128 MiB, so reaching
 * it costs a single function of more than 134 MB of emitted code — a real run,
 * on real hardware, taken once rather than every round. "Cannot run every round"
 * is not "cannot run", which is why this file exists instead of a note saying
 * the path is unwitnessable.
 *
 * ★★ THE SHAPE IS THE SAME ONE THE ±1 MiB EXAMPLE ESTABLISHED, AND DELIBERATELY
 * SO. A `do { } while` latch is the shape whose taken target is the loop HEAD
 * and whose fallthrough is the exit, so the latch emits the one-word
 * `b.cond <head>` form. Past ±1 MiB that word is promoted into the two-word
 * escape, whose trailing `b` carries the far target. Past ±128 MiB THAT word is
 * out of reach too, and the island path is what is left. So this program walks
 * the resolver's whole ladder in one function: in range, escaped, islanded.
 * ⓘ An `if (gate) { body }` does NOT walk it — ✔MEASURED at this tree, that
 * shape already emits the two-word form, so its `Imm19` never leaves range.
 *
 * ★ THE BODY IS A VOLATILE AGGREGATE COPY for the density and the
 * irreducibility: ~655 emitted bytes per statement at this tree, and `volatile`
 * forbids the optimizer eliding, merging or hoisting any of it, so the size is
 * a property of the source rather than of the pipeline that compiled it.
 * 262144 copies is ~171 MB, comfortably past the 134 MB edge — the margin is
 * there because the per-statement figure is a MEASUREMENT of one tree and will
 * move, and an entry sized to the exact edge would stop reaching the path the
 * first time codegen improved, silently.
 *
 * ⚠ THE SOURCE IS 507 BYTES OF CODE AND EXPANDS IN THE PREPROCESSOR. There is
 * no generated megabyte to ship, nothing to regenerate by hand, and the repeat
 * count is legible at the bottom of this file. ⓘ The expansion itself is a real
 * load on the preprocessor and is part of what this run measures.
 *
 * ⚠ WHAT STOPS THIS RUN, AND THE PREDICTION WAS WRONG IN A USEFUL WAY. This
 * note used to say peak MEMORY would bite first, because the resolver
 * re-encodes the whole function on each relaxation pass. ✔MEASURED on the first
 * real run (Windows host, cross-compiling, figures and conditions in
 * docs/branch-relaxation-limits.md): memory was never the wall. The peak was
 * ~9.03 GB, it arrived at about 255 s, and the resident set then FELL back to
 * ~150 MB while the CPU stayed pinned at 100% -- so the big allocation is the
 * IR and it is released before the phase that does not finish. The run produced
 * no artifact and no output at all after 73 minutes.
 * ⇒ THE BOUND ON THIS TIER IS COMPUTE, NOT MEMORY. Leave it running, and record
 * what it does: a completion, a loud A_FunctionEncodeAborted from `relaxBound`,
 * or another time bound. All three are results, and a run that is stopped is
 * reported as stopped rather than as a failure of this file.
 */

volatile int dss_lbi_gate;

struct wide { long a[32]; };
volatile struct wide dss_lbi_p;
volatile struct wide dss_lbi_q;

#define S          dss_lbi_p = dss_lbi_q;
#define R4(x)      x x x x
#define R16(x)     R4(R4(x))
#define R256(x)    R16(R16(x))
#define R4096(x)   R16(R256(x))
#define R65536(x)  R16(R4096(x))
#define R262144(x) R4(R65536(x))

int dss_lbi_island_latch(void) {
    do {
        R262144(S)
    } while (dss_lbi_gate);
    return 42;
}

int main(void) {
    return dss_lbi_island_latch();
}
